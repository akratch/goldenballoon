/**
 * gfx_pc_dkr.c — F3DDKR display-list interpreter (HLE) for mdkr64.
 *
 * Decodes Diddy Kong Racing's custom F3DDKR microcode command stream into modern
 * GPU draw calls via the shared GfxRenderingAPI backend (gfx_opengl.c /
 * gfx_metal.mm, reused verbatim from mgb64).
 *
 * PROVENANCE (see platform/fast3d/PROVENANCE.md): the RDP-side machinery here —
 * texture format decoders, the color-combiner→shader translation
 * (dkr_generate_cc), the per-vertex VBO packing that feeds the backend shaders,
 * the tile/TMEM model and the screen-rectangle path — is ported from
 * Emill/n64-fast3d-engine by way of mgb64's src/platform/fast3d/gfx_pc.c
 * (n64-fast3d-engine license, modified BSD-2-Clause). Diagnostics, GE007-specific
 * hacks and the base-GBI opcode decode of that file were dropped; the F3DDKR
 * command decode and the DKR vertex/triangle/matrix/billboard pipeline are new,
 * written from game/include/f3ddkr.h, game/include/structs.h and the DKR decomp
 * call sites (objects.c, particles.c, camera.c, textures_sprites.c, rcp_dkr.c).
 * Command semantics cross-checked against GLideN64 src/uCodes/F3DDKR.cpp.
 *
 * ENDIANNESS / LAYOUT: per PLAN.md decision 7, all in-memory DL / vertex /
 * triangle / matrix data is assumed NATIVE-endian (a separate asset-load
 * workstream byteswaps ROM data). This file therefore reads struct fields
 * directly. It also reads through the compiled struct TYPES (Vertex, Triangle,
 * Gfx, Mtx) rather than hardcoded byte strides, so it stays correct whatever
 * width `u32` resolves to (see the report's note on ultratypes.h).
 *
 * LP64 STRUCT-SIZE LOCKS (critical): the on-wire DKR command/data structs must
 * keep their exact N64 byte size on every host width, or the game-side buffer
 * layout math desyncs from the writer/reader stride. Two members are N64 `long`
 * (32-bit) that inflate to 8 bytes on LP64 and are pinned back to 32-bit under
 * NATIVE_PORT in PR/gbi.h:
 *   - Mtx_t (matrix element) — else Mtx is 128 B not 64 B.
 *   - Gsetcolor.color (a Gfx union member) — else `sizeof(Gfx)` is 16 not 8.
 * The `sizeof(Gfx)==8` lock matters because game code builds DL buffers whose DL
 * region is sized with a LITERAL 8-byte Gfx stride (e.g. tex_load_sprite reserves
 * `numTextures * 0x20` == 4*sizeof(Gfx) per tile) while the DL WRITER advances by
 * the real `sizeof(Gfx)`. If they disagree, the DL region over-runs into the
 * following VERTEX region and sprite quads read back Gfx-command bytes — the
 * billboard "red spikes / thin bars" over karts and menu sprites. (Fixed at
 * source; this interpreter reads Gfx at whatever stride the type reports, so it
 * was consistent with the writer either way — the corruption was purely the
 * game-side literal-vs-sizeof mismatch caused by the inflated Gfx.)
 *
 * ADDRESS RESOLUTION: every pointer embedded in a DL word (w1) is a 32-bit
 * physical/tagged value (the Gfx word is `unsigned int`, i.e. truncated on
 * LP64). We resolve it with gfx_resolve_addr() from platform/gfx_ptr.h, which
 * consults the segment table then the truncated→full pointer registry. The
 * OS-shim workstream owns the store side: it must register every host pointer
 * that reaches a DL word via gfx_ptr_store() (PLAN decision 3).
 */

#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>
#include "f3ddkr.h"      /* G_TRIN, G_DMADL, G_MW_BILLBOARD, G_MW_MVPMATRIX, ... */
#include "structs.h"     /* Vertex (10B), Triangle (0x10), TexCoords */

#include "gfx_rendering_api.h"
#include "gfx_cc.h"
#include "gfx_palette.h"
#include "gfx_screen_config.h"
#include "gfx_ptr.h"
#include "platform_os.h"   /* dkr_lo32_to_ptr + arena bounds (address resolution) */
#include "display_config.h"

#include "gfx_mipgen.h"
#include "gfx_texture_cache_key.h"
#include "gfx_texture_edge.h"
#include "gfx_font_sdf.h"
#include "gfx_level_lighting.h"
#include "gfx_rl1_experiment.h"
#include "gfx_render_scale.h"
#include "gfx_rdp_interpolation.h"
#include "gfx_presentation_packet.h"
#include "gfx_retained_task.h"
#include "gfx_shadow_frame.h"
#include "presentation_snapshot.h"
#include "present_sched.h"   /* presentation-replay arming seam */
#include "gfx_uniforms.h"
#include "gfx_pc_dkr.h"
#ifdef MDKR_WEBGPU_BACKEND
#include "gfx_webgpu.h"
#endif

/* ------------------------------------------------------------------------- */
/* Configuration / constants                                                 */
/* ------------------------------------------------------------------------- */

#define DKR_MAX_VERTICES   32          /* DKR RSP internal vertex buffer size */
#define DKR_VTX_SCRATCH    (DKR_MAX_VERTICES + 4) /* +4 for rectangle corners */
#define DKR_DEFORMATION_OWNERS PRESENTATION_SNAPSHOT_MAX_OBJECTS
#define DKR_MAX_BUFFERED   2048        /* triangles buffered before a flush   */
#define DKR_VBO_STRIDE_MAX 32          /* floats per vertex (generous)        */
#define DKR_DL_MAX_DEPTH   16          /* nested G_DL / G_DMADL recursion cap */
#define DKR_FONT_UPSCALE   4

enum { DKR_PRESENTATION_PARTICLE_KIND_POINT = 4 };

/* N64 5/4/3-bit channel → 8-bit expansions (Emill fast3d). */
#define SCALE_5_8(v_) (((v_) * 0xFF) / 0x1F)
#define SCALE_4_8(v_) ((v_) * 0x11)
#define SCALE_3_8(v_) ((v_) * 0x24)

static GfxFontRegistry dkr_font_registry;

uint32_t gfx_dkr_font_sdf_uploads;
uint32_t gfx_dkr_mipmapped_uploads;
uint64_t gfx_dkr_mip_levels_uploaded;
uint32_t gfx_dkr_font_registry_failures;
uint64_t gfx_dkr_rl1_triangles;
uint64_t gfx_dkr_remaster_racer_triangles;
uint64_t gfx_dkr_remaster_character_triangles;
uint64_t gfx_dkr_remaster_missing_normal_batches;
uint64_t gfx_dkr_shadow_receiver_invalid_world;
uint64_t gfx_dkr_texture_ids_created;
uint64_t gfx_dkr_texture_ids_deleted;
uint32_t gfx_dkr_texture_ids_live;
uint32_t gfx_dkr_texture_ids_high_water;
uint64_t gfx_dkr_shader_programs_created;

static struct {
    bool active;
    int32_t level;
    int32_t players;
    int32_t cutscene;
    uint64_t texture_created_start;
    uint64_t texture_deleted_start;
    uint64_t shader_created_start;
    uint32_t texture_live_start;
    uint32_t texture_live_peak;
} dkr_resource_generation;
static uint64_t dkr_shadow_stage_generation;

bool gfx_dkr_font_texture_register(
    const void *source, const GfxFontAtlasRegion *regions,
    size_t region_count) {
    bool registered;

    if (source == NULL) {
        return false;
    }
    /*
     * Region metadata is part of the derived image. Drop an earlier upload
     * before merging a shared font's cells so the next bind cannot reuse an
     * atlas derived from an incomplete registry entry.
     */
    gfx_dkr_texcache_invalidate_range(source, 1);
    registered = gfx_font_registry_register(
        &dkr_font_registry, source, regions, region_count);
    if (!registered) {
        gfx_dkr_font_registry_failures++;
    }
    return registered;
}

bool gfx_dkr_font_texture_unregister(const void *source) {
    bool unregistered;

    if (source == NULL) {
        return false;
    }
    gfx_dkr_texcache_invalidate_range(source, 1);
    unregistered =
        gfx_font_registry_unregister(&dkr_font_registry, source);
    if (!unregistered) {
        gfx_dkr_font_registry_failures++;
    }
    return unregistered;
}

static bool dkr_font_sdf_enabled(void) {
    static int enabled = -1;

    if (enabled < 0) {
        const char *value = getenv("MDKR_FONT_SDF");
        enabled = !(value != NULL &&
                    (value[0] == '0' ||
                     ((value[0] == 'o' || value[0] == 'O') &&
                      (value[1] == 'f' || value[1] == 'F'))));
    }
    return enabled != 0;
}

static bool dkr_native_ui_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("MDKR_UI_NATIVE_RES");
        enabled = !(value != NULL &&
                    (value[0] == '0' ||
                     ((value[0] == 'o' || value[0] == 'O') &&
                      (value[1] == 'f' || value[1] == 'F'))));
    }
    return enabled != 0;
}

/* Bitfield extract from the two DL words (Emill fast3d convention). */
#define C0(cmd, pos, sz) (((cmd)->words.w0 >> (pos)) & ((1U << (sz)) - 1))
#define C1(cmd, pos, sz) (((cmd)->words.w1 >> (pos)) & ((1U << (sz)) - 1))

/* ------------------------------------------------------------------------- */
/* DL opcode tracing (MDKR_TRACE=2)                                           */
/* ------------------------------------------------------------------------- */
/* Per-frame, opcode-level diagnostic dump of the F3DDKR command stream — the
 * primary renderer bring-up diagnostic beside the dumped frame images. Enabled
 * when MDKR_TRACE>=2. Optionally restrict to one frame index with MDKR_DL_FRAME
 * (0-based, matching --dump-frames numbering) to keep the log tractable. */
static int  dkr_trace2 = -1;    /* -1 unknown, 0 off, 1 on */
static int  dkr_trace_frame_filter = -2; /* -2 unread, -1 all frames, >=0 one */
static int  dkr_frame_index = -1;
static bool dkr_trace_this_frame = false;

/* ------------------------------------------------------------------------- */
/* Presentation replay                                                        */
/* ------------------------------------------------------------------------- */
/*
 * True while the HLE is walking a display list for the SECOND time in order to
 * present an additional (optionally camera-interpolated) image from the tick
 * that built it. Everything a second walk would otherwise double is gated on
 * this: the display-list census, the frame index the tracers key off, and — via
 * gfx_shadow_capture_suppress — the caster capture. The walk itself is
 * deliberately unchanged; the only behavioural difference is in G_MTX, where a
 * registered matrix is recomposed from its (world, view_projection)
 * decomposition instead of decoded from the display list's baked bytes.
 *
 * The captured display list is never written to. rsp.mtx is filled from a
 * matrix recomposed on the stack, so hashing or debugging the real tick's
 * display-list memory is unaffected, and nothing is written back into game
 * memory: a replay is required to be observationally identical to the tick it
 * replays, apart from the presented image.
 */
static bool dkr_replay_pass = false;
static bool dkr_replay_dependency_failed = false;
static bool dkr_replay_object_alpha_valid = false;
static uint64_t dkr_replay_object_alpha_numerator = 0;
static uint64_t dkr_replay_object_alpha_denominator = 1;
static int dkr_replay_object_interpolation = -1;
static int dkr_replay_deformation_interpolation = -1;
static int dkr_replay_projected_shadow_interpolation = -1;
static int dkr_test_projected_shadow_vertex_lerp = -1;
static int dkr_replay_particle_interpolation = -1;
static int dkr_replay_vertex_color_interpolation = -1;
static int dkr_replay_primitive_alpha_interpolation = -1;
static int dkr_replay_effect_interpolation = -1;
static int dkr_replay_uv_scroll_interpolation = -1;
static int dkr_test_live_arena_poison = -1;
static int dkr_test_endpoint_vertex_bytes = -1;
static Gfx *dkr_last_walked_dl = NULL;
static uint64_t dkr_last_walked_authored_tick = 0u;
static uint64_t dkr_future_last_published_tick = UINT64_MAX;
static uint64_t dkr_replay_matrix_hits = 0;
static uint64_t dkr_replay_matrix_misses = 0;
static uint64_t dkr_replay_object_hits = 0;
static uint64_t dkr_replay_object_holds = 0;
static uint64_t dkr_replay_billboard_matrix_hits = 0;
static uint64_t dkr_replay_billboard_matrix_holds = 0;
static uint64_t dkr_replay_billboard_vertex_hits = 0;
static uint64_t dkr_replay_billboard_vertex_holds = 0;
/*
 * Registry hits refused because the Mtx address had been rewritten since it was
 * registered (see dkr_shadow_lookup_live). Counted on BOTH walks, not just the
 * replay: a dead tenant feeds the shadow caster on the real walk too, which is
 * where the visual defect lived.
 */
static uint64_t dkr_shadow_stale_tenants = 0;
static uint64_t dkr_replay_matrix_rejects = 0;
static uint64_t dkr_replay_matrix_tolerant = 0;
static uint64_t dkr_replay_matrix_tolerant_worst = 0;
static uint64_t dkr_replay_matrix_reject_least = 0;
static bool dkr_replay_matrix_reject_least_set = false;
static bool dkr_replay_force_recompose = false;
static int dkr_test_recompose_reject = -1;
static bool dkr_test_recompose_reject_used = false;
static int dkr_test_framed_world_unsafe = -1;
static uint64_t dkr_replay_walks = 0;

/*
 * RECOMPOSITION TOLERANCE, in s15.16 LSBs. One LSB is 1/65536 of a world unit,
 * so 4096 is 1/16 of a world unit.
 *
 * The replay proves a registry entry's (world, view_projection) decomposition by
 * recomposing with the view-projection AS CAPTURED and comparing against the
 * display list's own matrix. That comparison used to be bit-exact, and bit-exact
 * cannot tell "the same product, re-associated by one rounding step" from "the
 * wrong association, thousands of world units away". In the 63-level
 * presentation sweep, both populations are
 * present and they do not overlap remotely:
 *
 *   - float re-association, 99.96% of all mismatches, worst a few thousand LSBs
 *     (hundredths of a world unit). mtx_head_rotation (camera.c) builds its list
 *     matrix as head x (parentWorld x VP) but must register a world of
 *     (head x parentWorld); the two products are equal in exact arithmetic.
 *   - genuine mis-association, historically millions of LSBs. Production now
 *     refuses dead matrix-registry tenants before this comparison, and the
 *     owner-specific presentation packet can supersede some stale bindings,
 *     so the registered breadth gate injects one deterministic 2,000,000-LSB
 *     verification error to keep the rejection side load-bearing.
 *
 * The injected control is hundreds of times above 4096 and tens of thousands
 * of times above the measured accepted tail, so the threshold remains in empty
 * space rather than fitted to either edge.
 * gfx_dkr_replay_get_reject_stats() reports the worst accepted and the least
 * rejected magnitude of every run, so the margin is measured continuously
 * rather than assumed from this comment.
 *
 * Applied ONLY when the entry's view-projection was actually overridden -- see
 * the G_MTX replay branch. With the camera unchanged the display list already
 * holds the exact matrix and recomposing could only lose precision to it, which
 * is what check_render_purity.py's arm E asserts by comparing pixels.
 */
#define DKR_RECOMPOSE_TOLERANCE_LSB 4096

bool gfx_dkr_replay_pass_active(void) { return dkr_replay_pass; }

/*
 * Worst per-element absolute difference between two Mtx images, in s15.16 LSBs.
 *
 * A Mtx is NOT sixteen s15.16 words. mtxf_to_mtx (math_util.c) splits each
 * element into an integer half stored in the first eight words and a fractional
 * half stored in the last eight, TWO ELEMENTS TO A WORD. Differencing the words
 * directly reports 2^32 for an element that merely crossed zero, so both
 * matrices are un-packed back to s15.16 before they are compared. Every caller
 * of this function is making a geometric judgement, so none of them may use the
 * packed form.
 */
static int64_t dkr_mtx_worst_lsb_delta(const void *listed_bytes,
                                       const void *built_bytes) {
    const uint32_t *listed = (const uint32_t *)listed_bytes;
    const uint32_t *built = (const uint32_t *)built_bytes;
    int64_t worst = 0;
    for (int pair = 0; pair < 8; pair++) {
        uint32_t hi_a = listed[pair];
        uint32_t lo_a = listed[pair + 8];
        uint32_t hi_b = built[pair];
        uint32_t lo_b = built[pair + 8];
        int32_t element_a[2];
        int32_t element_b[2];
        element_a[0] = (int32_t)((hi_a & 0xFFFF0000u) | ((lo_a >> 16) & 0xFFFFu));
        element_a[1] = (int32_t)(((hi_a & 0xFFFFu) << 16) | (lo_a & 0xFFFFu));
        element_b[0] = (int32_t)((hi_b & 0xFFFF0000u) | ((lo_b >> 16) & 0xFFFFu));
        element_b[1] = (int32_t)(((hi_b & 0xFFFFu) << 16) | (lo_b & 0xFFFFu));
        for (int half = 0; half < 2; half++) {
            int64_t delta = (int64_t)element_b[half] - (int64_t)element_a[half];
            if (delta < 0) {
                delta = -delta;
            }
            if (delta > worst) {
                worst = delta;
            }
        }
    }
    return worst;
}

/*
 * MDKR_TEST_RECOMPOSE_REJECT=1 is the deterministic broken direction for the
 * geometric tolerance. The old control depended on a particular allocator
 * address being reused by an unregistered slot-2 matrix; owner-specific replay
 * can now replace that stale world before verification, so the natural defect
 * is correctly harmless and no longer a stable failure injector.
 *
 * On the first eligible replay matrix, force element zero of the verification
 * image exactly 2,000,000 s15.16 LSBs away from the display-list image. This is
 * ~30.5 world units, far above the 4,096-LSB tolerance and far below integer
 * overflow. Only the stack-local verification copy changes; the display list,
 * decoded draw matrix, game state, and production path are untouched.
 */
#define DKR_TEST_RECOMPOSE_REJECT_LSB 2000000

static bool dkr_test_recompose_reject_enabled(void) {
    if (dkr_test_recompose_reject < 0) {
        const char *value = getenv("MDKR_TEST_RECOMPOSE_REJECT");
        dkr_test_recompose_reject =
            value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    }
    return dkr_test_recompose_reject != 0;
}

static bool dkr_test_framed_world_unsafe_enabled(void) {
    if (dkr_test_framed_world_unsafe < 0) {
        const char *value = getenv("MDKR_TEST_FRAMED_WORLD_UNSAFE");
        dkr_test_framed_world_unsafe =
            value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    }
    return dkr_test_framed_world_unsafe != 0;
}

static void dkr_mtx_inject_recompose_reject(const void *listed_bytes,
                                            void *built_bytes) {
    const uint32_t *listed = (const uint32_t *)listed_bytes;
    uint32_t *built = (uint32_t *)built_bytes;
    int32_t listed_element = (int32_t)(
        (listed[0] & 0xFFFF0000u) | ((listed[8] >> 16) & 0xFFFFu));
    int64_t target = (int64_t)listed_element + DKR_TEST_RECOMPOSE_REJECT_LSB;

    if (target > INT32_MAX) {
        target = (int64_t)listed_element - DKR_TEST_RECOMPOSE_REJECT_LSB;
    }
    built[0] = (built[0] & 0x0000FFFFu) |
        ((uint32_t)(int32_t)target & 0xFFFF0000u);
    built[8] = (built[8] & 0x0000FFFFu) |
        (((uint32_t)(int32_t)target & 0x0000FFFFu) << 16);
}

/* game/src/camera.c (NATIVE_PORT): world x view_projection through the exact
 * mtxf_mul/mtxf_to_mtx pair the display-list build used, so a replay of an
 * unchanged view_projection is bit-identical rather than merely close. */
extern void mdkr_camera_replay_mvp(
    const float world[4][4], const float view_projection[4][4], void *out_mtx);
extern bool mdkr_camera_replay_object_world(
    const GfxPresentationMatrixOwner *owner, uint64_t numerator,
    uint64_t denominator, float out_world[4][4]);
extern bool mdkr_camera_replay_effect_world(
    const GfxPresentationMatrixOwner *previous,
    const GfxPresentationMatrixOwner *current, uint64_t numerator,
    uint64_t denominator, float out_world[4][4]);
extern bool mdkr_camera_replay_billboard_anchor(
    const GfxPresentationMatrixOwner *owner, uint64_t numerator,
    uint64_t denominator, float out_position[3]);
extern bool mdkr_camera_replay_billboard_matrix(
    const GfxPresentationMatrixOwner *owner, int viewport,
    uint64_t numerator, uint64_t denominator, void *out_mtx);

static inline bool dkr_dl_trace_on(void) {
    if (dkr_trace2 < 0) {
        dkr_trace2 = (mdkr_trace_level() >= 2) ? 1 : 0;
        const char *f = getenv("MDKR_DL_FRAME");
        dkr_trace_frame_filter = (f && f[0]) ? atoi(f) : -1;
    }
    if (!dkr_trace2) return false;
    if (dkr_trace_frame_filter >= 0 && dkr_frame_index != dkr_trace_frame_filter)
        return false;
    return true;
}

static bool dkr_replay_object_interpolation_enabled(void) {
    if (dkr_replay_object_interpolation < 0) {
        const char *value = getenv("MDKR_TEST_OBJECT_INTERPOLATION");
        dkr_replay_object_interpolation =
            !(value != NULL &&
              (strcmp(value, "off") == 0 || strcmp(value, "0") == 0));
    }
    return dkr_replay_object_interpolation != 0;
}

static bool dkr_replay_deformation_interpolation_enabled(void) {
    if (dkr_replay_deformation_interpolation < 0) {
        const char *value = getenv("MDKR_TEST_DEFORMATION_INTERPOLATION");
        dkr_replay_deformation_interpolation =
            !(value != NULL &&
              (strcmp(value, "off") == 0 || strcmp(value, "0") == 0));
    }
    return dkr_replay_deformation_interpolation != 0;
}

static bool dkr_replay_projected_shadow_interpolation_enabled(void) {
    if (dkr_replay_projected_shadow_interpolation < 0) {
        const char *value =
            getenv("MDKR_TEST_PROJECTED_SHADOW_INTERPOLATION");
        dkr_replay_projected_shadow_interpolation =
            !(present_sched_internal_replay_test_enabled() && value != NULL &&
              (strcmp(value, "off") == 0 || strcmp(value, "0") == 0));
    }
    return dkr_replay_projected_shadow_interpolation != 0;
}

static bool dkr_test_projected_shadow_vertex_lerp_enabled(void) {
    if (dkr_test_projected_shadow_vertex_lerp < 0) {
        const char *value =
            getenv("MDKR_TEST_PROJECTED_SHADOW_VERTEX_LERP");
        dkr_test_projected_shadow_vertex_lerp =
            present_sched_internal_replay_test_enabled() && value != NULL &&
            value[0] != '\0' && strcmp(value, "0") != 0;
    }
    return dkr_test_projected_shadow_vertex_lerp != 0;
}

static bool dkr_replay_particle_interpolation_enabled(void) {
    if (dkr_replay_particle_interpolation < 0) {
        const char *value = getenv("MDKR_TEST_PARTICLE_INTERPOLATION");
        dkr_replay_particle_interpolation =
            !(value != NULL &&
              (strcmp(value, "off") == 0 || strcmp(value, "0") == 0));
    }
    return dkr_replay_particle_interpolation != 0;
}

static bool dkr_replay_vertex_color_interpolation_enabled(void) {
    if (dkr_replay_vertex_color_interpolation < 0) {
        const char *value = getenv("MDKR_TEST_VERTEX_COLOR_INTERPOLATION");
        dkr_replay_vertex_color_interpolation =
            !(value != NULL &&
              (strcmp(value, "off") == 0 || strcmp(value, "0") == 0));
    }
    return dkr_replay_vertex_color_interpolation != 0;
}

static bool dkr_replay_primitive_alpha_interpolation_enabled(void) {
    if (dkr_replay_primitive_alpha_interpolation < 0) {
        const char *value = getenv(
            "MDKR_TEST_PRIMITIVE_ALPHA_INTERPOLATION");
        dkr_replay_primitive_alpha_interpolation =
            !(value != NULL &&
              (strcmp(value, "off") == 0 || strcmp(value, "0") == 0));
    }
    return dkr_replay_primitive_alpha_interpolation != 0;
}

static bool dkr_replay_effect_interpolation_enabled(void) {
    if (dkr_replay_effect_interpolation < 0) {
        const char *value = getenv("MDKR_TEST_EFFECT_INTERPOLATION");
        dkr_replay_effect_interpolation =
            !(value != NULL &&
              (strcmp(value, "off") == 0 || strcmp(value, "0") == 0));
    }
    return dkr_replay_effect_interpolation != 0;
}

static bool dkr_replay_uv_scroll_interpolation_enabled(void) {
    if (dkr_replay_uv_scroll_interpolation < 0) {
        const char *value = getenv("MDKR_TEST_UV_SCROLL_INTERPOLATION");
        dkr_replay_uv_scroll_interpolation =
            !(value != NULL &&
              (strcmp(value, "off") == 0 || strcmp(value, "0") == 0));
    }
    return dkr_replay_uv_scroll_interpolation != 0;
}

static bool dkr_test_live_arena_poison_enabled(void) {
    if (dkr_test_live_arena_poison < 0) {
        const char *value = getenv("MDKR_TEST_RETAINED_ARENA_POISON");
        dkr_test_live_arena_poison =
            present_sched_internal_replay_test_enabled() && value != NULL &&
            value[0] != '\0' && strcmp(value, "0") != 0;
    }
    return dkr_test_live_arena_poison != 0;
}

static bool dkr_test_endpoint_vertex_bytes_enabled(void) {
    if (dkr_test_endpoint_vertex_bytes < 0) {
        const char *value = getenv("MDKR_TEST_ENDPOINT_VERTEX_BYTES");
        dkr_test_endpoint_vertex_bytes =
            value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    }
    return dkr_test_endpoint_vertex_bytes != 0;
}
#define DTRACE(...) do { if (dkr_trace_this_frame) { \
    fprintf(stderr, "[DL] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ------------------------------------------------------------------------- */
/* Shared front-end state                                                    */
/* ------------------------------------------------------------------------- */

/*
 * OUTPUT resolution — the drawable/surface, before Video.RenderScale.
 * gfx_current_dimensions is the RENDER resolution (output x scale); everything
 * that draws works in that space and the backend resolve is the one place the
 * frame comes back to this one. Backends must configure their swapchain and
 * size their readback target from THIS, never from gfx_current_dimensions.
 */
struct GfxDimensions gfx_output_dimensions = { DESIRED_SCREEN_WIDTH, DESIRED_SCREEN_HEIGHT,
                                               (float)DESIRED_SCREEN_WIDTH / (float)DESIRED_SCREEN_HEIGHT };

struct GfxDimensions gfx_current_dimensions = { DESIRED_SCREEN_WIDTH, DESIRED_SCREEN_HEIGHT,
                                                (float)DESIRED_SCREEN_WIDTH / DESIRED_SCREEN_HEIGHT };

static struct GfxRenderingAPI *gfx_rapi;
static bool dkr_output_overlay_active;
static bool dkr_output_overlay_suppressed;
static uint32_t dkr_output_overlay_frame_draws;
static uint32_t dkr_output_overlay_late_world_draws;
static uint64_t dkr_output_overlay_frames;
static uint64_t dkr_output_overlay_begin_failures;
static uint64_t dkr_primitive_serial;
static uint64_t dkr_last_world_primitive;
static bool dkr_primitive_prepared;
static bool dkr_primitive_overlay_candidate;
static int dkr_shadow_receiver_view = -1;
#ifndef __EMSCRIPTEN__
extern int gfx_opengl_max_offscreen_dim(void);
#endif
#ifdef MDKR_WEBGPU_BACKEND
extern int gfx_webgpu_max_offscreen_dim(void);
extern bool gfx_webgpu_unclipped_depth_supported(void);
extern bool gfx_webgpu_get_output_size(int *width, int *height);
#endif

/*
 * The AUTHORED surface: the logical framebuffer every display-list coordinate
 * (viewport, scissor, texrect, fill rectangle) is expressed in.
 *
 * NTSC and MPAL compose against 320x240, but PAL raises the framebuffer to
 * 320x264 (video.c adds PAL_HEIGHT_DIFFERENCE to every mode) and the game then
 * lays its menus out over all 264 rows -- gTrackSelectViewportY, the ortho
 * viewport's vtrans, the PAL text offsets and the full-surface scissor are all
 * in that taller space. Dividing those coordinates by a fixed 240 magnifies PAL
 * 2D art by 264/240 and walks everything below the vertical centre off the
 * bottom of the host surface, so the mapping tracks the live surface instead.
 * video.c publishes it through gfx_dkr_set_logical_surface().
 */
static float dkr_logical_width = (float)DESIRED_SCREEN_WIDTH;
static float dkr_logical_height = (float)DESIRED_SCREEN_HEIGHT;

struct RGBA { uint8_t r, g, b, a; };
struct XYWidthHeight { int32_t x, y, width, height; };
struct FloatXYWidthHeight { float x, y, width, height; };

static void dkr_remap_viewport_and_scissor(void);

struct LoadedVertex {
    float x, y, z, w;   /* clip-space (pre perspective divide) */
    float model_x, model_y, model_z; /* diagnostic geometric-normal source */
    float world_x, world_y, world_z; /* capture-once shadow/receiver position */
    bool world_valid;
    float normal_x, normal_y, normal_z; /* normalized object-space smooth normal */
    float light_x, light_y, light_z; /* object-space level sun, constant per draw */
    float u, v;         /* S10.5 texel coords for this corner (set per-triangle) */
    struct RGBA color;  /* shade colour */
    uint8_t fog;        /* per-vertex fog factor [0,255] */
};

struct DkrDeformationCursor {
    const void *address;
    uint64_t generation;
    int viewport;
    uint32_t next_stream;
};

/* ---- RSP-side state ---- */
static struct {
    float mtx[3][4][4];        /* 3 MVP matrix slots (G_MTX_DKR_INDEX_0..2)     */
    int   active_slot;         /* slot selected by gSPSelectMatrixDKR/gSPMatrix */
    bool  billboard;           /* gDkrEnableBillboard / gDkrDisableBillboard    */
    uint32_t geometry_mode;    /* G_ZBUFFER, G_FOG, G_CULL_*, ...               */
    int16_t fog_mul, fog_offset;
    uint16_t tex_scale_s, tex_scale_t; /* gSPTexture s/t scale (0.16 fixed)     */
    uint8_t  tile_base;        /* render tile index (G_TX_RENDERTILE)           */
    uint8_t  draw_space;       /* WORLD / SAFE_2D / FULLBLEED matrix tag         */
    bool world_safe_region;    /* framed world view uses the 4:3 safe rect       */
    uint8_t  remaster_light_class; /* 0 none, 1 racer, 2 character             */
    const Vec3s *smooth_normals; /* current compact ObjectModel normal subspan   */
    float light_direction[3];   /* normalized object-space direction TO sun     */
    GfxShadowMatrixBinding shadow_matrix[3];
    bool shadow_matrix_valid[3];
    /* The root/child whose model-space vertex stream is currently being
     * walked. The real walk and replay both start from the same zeroed RSP
     * state, so the per-root ordinal names the same G_VTX batch without
     * retaining a transient vertex pointer as identity. */
    GfxPresentationMatrixOwner deformation_owner;
    int deformation_viewport;
    uint32_t deformation_stream;
    uint32_t deformation_batch;
    bool deformation_owner_valid;
    GfxPresentationMatrixOwner opacity_owner;
    bool opacity_owner_valid;
    struct DkrDeformationCursor
        deformation_cursors[DKR_DEFORMATION_OWNERS];
    size_t deformation_cursor_count;
    int      vtx_append_pos;   /* append base: count of the last flag-0 load    */
    struct LoadedVertex loaded[DKR_VTX_SCRATCH];
} rsp;

static bool dkr_deformation_begin_stream(
    const GfxPresentationMatrixOwner *owner, int viewport) {
    struct DkrDeformationCursor *cursor = NULL;

    if (owner == NULL || !owner->valid || owner->address == NULL ||
        owner->generation == 0u) {
        return false;
    }
    for (size_t index = 0; index < rsp.deformation_cursor_count; index++) {
        struct DkrDeformationCursor *candidate =
            &rsp.deformation_cursors[index];
        if (candidate->address == owner->address &&
            candidate->generation == owner->generation &&
            candidate->viewport == viewport) {
            cursor = candidate;
            break;
        }
    }
    if (cursor == NULL) {
        if (rsp.deformation_cursor_count >= DKR_DEFORMATION_OWNERS) {
            return false;
        }
        cursor =
            &rsp.deformation_cursors[rsp.deformation_cursor_count++];
        memset(cursor, 0, sizeof(*cursor));
        cursor->address = owner->address;
        cursor->generation = owner->generation;
        cursor->viewport = viewport;
    }
    if (cursor->next_stream > UINT16_MAX) {
        return false;
    }
    rsp.deformation_stream = cursor->next_stream++;
    rsp.deformation_batch = 0u;
    return true;
}

static bool dkr_deformation_next_ordinal(uint32_t *out) {
    if (out == NULL || !rsp.deformation_owner_valid ||
        rsp.deformation_stream > UINT16_MAX ||
        rsp.deformation_batch > UINT16_MAX) {
        return false;
    }
    *out = (rsp.deformation_stream << 16) | rsp.deformation_batch++;
    return true;
}

/* Tile descriptor (SETTILE / SETTILESIZE). */
struct DkrTile {
    uint8_t fmt, siz;
    uint8_t cms, cmt;
    uint8_t masks, maskt;
    uint8_t shifts, shiftt;
    uint8_t palette;
    uint16_t uls, ult, lrs, lrt;
    uint16_t width, height;      /* texels, from SETTILESIZE               */
    uint16_t tmem;               /* 64-bit TMEM word index                 */
    uint32_t line_size_bytes;    /* row pitch, from SETTILE line*8         */
};

/* ---- RDP-side state ---- */
static struct {
    struct DkrTile tile[8];

    /* Loaded texture record, indexed by TMEM word slot (LOADBLOCK/LOADTILE). */
    struct {
        const uint8_t *addr;
        uint32_t size_bytes;
        bool line_swapped;       /* LOADBLOCK with dxt==0: odd source rows are
                                  * pre-word-swapped (see dkr_dp_load_block).   */
    } loaded_texture[512];

    /* Pending image pointer (SETTIMG). */
    struct { const uint8_t *addr; uint8_t siz; uint32_t width; } to_load;

    uint16_t palette[256];       /* TLUT (RGBA16 / IA16 entries)               */
    uint32_t palette_fmt;        /* G_TT_RGBA16 or G_TT_IA16                   */

    uint32_t other_mode_h, other_mode_l;
    uint64_t combine_mode;       /* normalized 28-bit-per-cycle cc_id          */

    struct RGBA env_color, prim_color, authored_prim_color, fog_color,
        fill_color;
    uint8_t prim_lod_fraction;

    struct XYWidthHeight viewport, scissor;
    /* Source rectangles in the game's 320x240 coordinate space, stored in
     * bottom-left coordinates.  A later tagged matrix can change draw space
     * after G_MOVEMEM/G_SETSCISSOR, so retain and remap instead of baking the
     * policy at command arrival time. */
    struct FloatXYWidthHeight logical_viewport, logical_scissor;
    bool logical_viewport_valid, logical_scissor_valid;
    bool viewport_flip_x;
    /* RSP clip-ratio emulation (see dkr_update_clip_expansion): the rectangle
     * actually handed to the backend, plus the clip-space scale/bias that keeps
     * the NDC->window map identical to the authored viewport's. Identity
     * whenever the authored viewport already covers the authored scissor. */
    struct XYWidthHeight clip_viewport;
    float clip_scale_x, clip_scale_y, clip_bias_x, clip_bias_y;
    bool clip_expanded;
    bool viewport_or_scissor_changed;
    const void *z_buf_address;
    const void *color_image_address;
    /* Raw SETCIMG/SETZIMG tokens (the DL w1 values, before dkr_resolve). The
     * z-clear detection compares THESE, not the resolved pointers: on wasm32 the
     * framebuffer/z-buffer segment tokens (0x01000000 / 0x02000000) can resolve
     * to NULL (they aren't registered like a >4GB LP64 host pointer is), which
     * would defeat a resolved-pointer comparison and draw the z-clear as a white
     * fill (the M3b symptom). The raw tokens are width-independent. */
    uint32_t z_buf_token;
    uint32_t color_image_token;
} rdp;

static void dkr_replay_apply_primitive_alpha(void) {
    PresentationObjectPose source;
    PresentationObjectPose target;
    bool particle;
    bool projected_shadow;
    uint8_t alpha;

    rdp.prim_color = rdp.authored_prim_color;
    if (!dkr_replay_pass || !dkr_replay_object_alpha_valid ||
        !dkr_replay_primitive_alpha_interpolation_enabled() ||
        !rsp.opacity_owner_valid || !rsp.opacity_owner.valid ||
        !presentation_snapshot_resolve_object_generation(
            rsp.opacity_owner.address, rsp.opacity_owner.generation,
            0u, dkr_replay_object_alpha_denominator, &source) ||
        !presentation_snapshot_resolve_object_generation(
            rsp.opacity_owner.address, rsp.opacity_owner.generation,
            dkr_replay_object_alpha_numerator,
            dkr_replay_object_alpha_denominator, &target) ||
        !target.interpolated) {
        return;
    }
    particle = source.is_particle != 0u;
    projected_shadow = rsp.opacity_owner.matrix_class ==
        GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
    /* Point trails carry their opacity in retained per-vertex alpha. Their
     * primitive alpha is deliberately 255 and must not be scaled a second
     * time. Line trails and sprite/model particles use primitive alpha. */
    if (particle &&
        source.model_index == DKR_PRESENTATION_PARTICLE_KIND_POINT) {
        return;
    }
    alpha = presentation_scale_opacity_u8(
        rdp.authored_prim_color.a, source.opacity, target.opacity);
    gfx_presentation_packet_note_primitive_alpha(
        particle, alpha != rdp.authored_prim_color.a);
    if (projected_shadow) {
        gfx_presentation_packet_note_projected_shadow_primitive_alpha(
            alpha != rdp.authored_prim_color.a);
    }
    rdp.prim_color.a = alpha;
}

static void dkr_replay_bind_opacity_owner(
    const GfxPresentationMatrixOwner *owner) {
    if (owner != NULL && owner->valid &&
        (owner->matrix_class == GFX_PRESENTATION_MATRIX_ROOT ||
         owner->matrix_class == GFX_PRESENTATION_MATRIX_CHILD ||
         owner->matrix_class == GFX_PRESENTATION_MATRIX_BILLBOARD ||
         owner->matrix_class ==
             GFX_PRESENTATION_MATRIX_PARTICLE_VERTICES ||
         owner->matrix_class ==
             GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES)) {
        rsp.opacity_owner = *owner;
        rsp.opacity_owner_valid = true;
    } else {
        memset(&rsp.opacity_owner, 0, sizeof(rsp.opacity_owner));
        rsp.opacity_owner_valid = false;
    }
    dkr_replay_apply_primitive_alpha();
}

/* True while emitting a textured screen rectangle. Set before material setup
 * so texture upload/cache decisions can distinguish text/2D from geometry. */
static bool dkr_in_texrect;

/*
 * The HLE's per-frame reset is deliberately PARTIAL. gfx_start_frame clears rsp
 * and a handful of rdp fields, but the rest of the RDP state — loaded tiles,
 * palettes, the segment table — persists from the previous walk, because that
 * is what the hardware did and what DKR's display lists assume: a list does not
 * re-establish everything it depends on, it inherits.
 *
 * A replay must therefore start from the state the REAL walk started from, not
 * from the state that walk left behind. Snapshot is taken in gfx_start_frame,
 * after the reset and before a single command is decoded, and only when a
 * replay is armed.
 */
static unsigned char dkr_walk_entry_rdp[sizeof(rdp)];
static unsigned char dkr_walk_entry_rsp[sizeof(rsp)];
static uintptr_t dkr_walk_entry_segments[16];
static bool dkr_walk_entry_texrect;
static bool dkr_walk_entry_valid = false;

/* ---- Backend-mirrored rendering state (avoid redundant API calls) ---- */
static struct {
    uint8_t depth_mode;
    enum GfxBlendMode blend_mode;
    /* viewport here is the EFFECTIVE (clip-expanded) rectangle the backend was
     * given, not rdp.viewport; the clip factors travel with it because the VBO
     * contents depend on them. */
    struct XYWidthHeight viewport, scissor;
    float clip_scale_x, clip_scale_y, clip_bias_x, clip_bias_y;
    struct ShaderProgram *shader_program;
    uint32_t bound_texture_id[2];
    bool bound_texture_linear[2];
    uint8_t bound_texture_cms[2], bound_texture_cmt[2];
    /* Part of the sampler memo key. Without it a 2D draw following a 3D draw
     * with the same filter/wrap would skip set_sampler_parameters entirely and
     * silently keep the mipmapped sampler. */
    bool bound_texture_lod0[2];
} rendering_state;

/* ------------------------------------------------------------------------- */
/* VBO buffer                                                                */
/* ------------------------------------------------------------------------- */

static float  buf_vbo[DKR_MAX_BUFFERED * 3 * DKR_VBO_STRIDE_MAX];
static size_t buf_vbo_len;
static size_t buf_vbo_num_tris;

/* ------------------------------------------------------------------------- */
/* Address resolution                                                        */
/* ------------------------------------------------------------------------- */

/* Resolve a DL-embedded 32-bit address. Returns NULL when unresolvable — every
 * caller null-checks.
 *
 * mdkr64 address scheme (RECONCILED, PLAN decision 3): game code writes host
 * pointers into DL words truncated to 32 bits (osVirtualToPhysical returns
 * (u32)ptr). All asset data is DMA'd into the size-aligned RDRAM-stand-in arena,
 * so the low 32 bits losslessly reconstruct via dkr_lo32_to_ptr() (arena high
 * bits OR'd back). Try that FIRST and accept it only when the reconstructed
 * pointer actually lands inside the arena; otherwise fall through to
 * gfx_resolve_addr() for segment tokens (gSPSegment / G_MW_SEGMENT sets, whose
 * bases live in gfx_segment_table) and any non-arena host pointer parked in the
 * gfx_ptr registry. Never returns a wild pointer — worst case is NULL. */
/* A pointer is host-plausible if it is non-null and not a sign-extended 32-bit
 * token (high 32 bits all ones). User-space host mappings never live at
 * 0xffffffff........, and the arena's high bits are 0x4-0x7, so an all-ones high
 * half is unambiguously a truncated-then-sign-extended pointer. dkr_resolve
 * refuses to hand such a value to any DL consumer — the belt-and-suspenders side
 * of the char-select SIGSEGV fix (the truncation itself is fixed at its source
 * in tracks.c render_level_segment). */
static inline bool dkr_ptr_plausible(const void *p) {
    uintptr_t up = (uintptr_t) p;
    if (up == 0) return false;
#if UINTPTR_MAX > UINT32_MAX
    if ((up >> 32) == UINT32_MAX) return false;
#endif
    return true;
}

static inline void *dkr_retain_resolved_pointer(void *resolved) {
    const void *retained = NULL;

    if (!dkr_ptr_plausible(resolved)) {
        return NULL;
    }
    if (dkr_replay_pass && gfx_retained_task_lookup_dependency(
            resolved, 1u, &retained)) {
        return (void *)retained;
    }
    return resolved;
}

static inline void *dkr_resolve(uint32_t addr) {
    if (addr == 0) {
        return NULL;
    }
    /* THE address encoding (verified via DL tracing): DKR builds nearly every DL
     * pointer with OS_K0_TO_PHYSICAL(ptr) = (u32)((char*)ptr - 0x80000000), or the
     * equivalent `(s32)ptr + K0BASE` in the segment setup. Because 0x80000000 has
     * only bit 31 set, subtracting or adding it mod 2^32 just TOGGLES bit 31, so
     * every such token is exactly `(u32)hostptr ^ 0x80000000`. A few paths use the
     * raw form `(u32)ptr` (osVirtualToPhysical, raw gSPDisplayList). We therefore
     * try both the flipped and raw keys against both resolution strategies.
     *
     * ORDER MATTERS — registry FIRST, arena reconstruction SECOND:
     *   - The registry holds the EXACT full host pointer for every non-arena
     *     pointer the game converted (globals / rodata DLs — gViewportStack, the
     *     matrix stack, dMenuHudDrawModes, dRdpInit). It is authoritative.
     *   - Arena reconstruction (OR the arena's high bits onto the low 32) is a
     *     heuristic: a GLOBAL's low 32 bits can coincidentally land inside the
     *     16 MB arena window, so reconstructing first would hand back arena
     *     garbage (e.g. an all-zero region) instead of the real global — which is
     *     exactly what collapsed the matrix stack to zeros. Consulting the
     *     registry first resolves those globals correctly; arena tokens (vertex/
     *     triangle/matrix data built in the arena) are NOT registered, miss the
     *     registry, and fall through to reconstruction as intended.
     *   - Genuine N64 segment tokens (framebuffer/zbuffer 0x0N000000) are neither
     *     registered nor arena-reconstructable and resolve via the segment table
     *     last, so a global's segment-nibble collision can never pre-empt it. */
    uint32_t flip = addr ^ 0x80000000u;
    void *r = gfx_ptr_resolve(flip);          /* OS_K0_TO_PHYSICAL globals */
    if (dkr_ptr_plausible(r)) {
        return dkr_retain_resolved_pointer(r);
    }
    r = gfx_ptr_resolve(addr);                /* raw gDma1p-registered globals */
    if (dkr_ptr_plausible(r)) {
        return dkr_retain_resolved_pointer(r);
    }

    /* During presentation replay the live arena may already contain task
     * K+1. Decode task K's tokens against its retained original window and
     * return the corresponding private bytes before considering the live
     * arena. Registry/global resolution stays first to preserve its collision
     * rule (see the ORDER MATTERS note above). */
    if (dkr_replay_pass) {
        r = gfx_retained_task_resolve_arena_token(addr);
        if (dkr_ptr_plausible(r)) {
            return r;
        }
    }

    uintptr_t base = (uintptr_t) g_dkrArenaBase;
    uintptr_t end  = base + (uintptr_t) g_dkrArenaSize;
    uint32_t cand[2] = { flip, addr };
    for (int i = 0; i < 2; i++) {
        void *p = (void *)(g_dkrArenaHi | (uintptr_t) cand[i]);
        uintptr_t up = (uintptr_t) p;
        if (up >= base && up < end) {
            return p;
        }
    }
#if UINTPTR_MAX == UINT32_MAX
    /* ILP32 DIRECT RECOVERY. Pointers are 32-bit, so a host-pointer token
     * recovers the real address by construction — unlike LP64, where the host high
     * bits are lost on truncation and must come from the gfx_ptr registry. DKR
     * builds DL pointers as OS_K0_TO_PHYSICAL(p)=p^0x80000000 (bit 31 set) or the
     * raw (u32)p / `(s32)p+K0BASE` forms; globals/rodata live below the arena,
     * which is forced above the 0x10000000 segment ceiling
     * (dkr_arena_init), and N64 segment tokens are 0x01000000..0x0FFFFFFF. So:
     *   - bit 31 set  -> a flipped real pointer; XOR it back to the host address.
     *   - < 0x01000000 -> a raw low host pointer (global/rodata; segment-0 base is
     *                     0 so a segment-0 offset is the same value).
     *   - 0x01000000..0x0FFFFFFF -> a genuine segment token; fall to the table.
     * The registry/arena checks above still run first (and win for anything they
     * resolve), so this only catches the globals LP64 recovers via the registry.
     * Keying this to pointer width, not Emscripten, gives every supported ILP32
     * host the same non-colliding token domains. */
    if (addr >= 0x80000000u) {
        return flip
            ? dkr_retain_resolved_pointer((void *)(uintptr_t)flip) : NULL;
    }
    if (addr != 0 && addr < 0x01000000u) {
        return dkr_retain_resolved_pointer((void *)(uintptr_t)addr);
    }
#endif
    r = gfx_resolve_addr(addr);   /* genuine N64 segment tokens */
    return dkr_retain_resolved_pointer(r);
}

/* Bytes readable from `p` without leaving the arena or a private retained
 * external span. Returns SIZE_MAX for ordinary non-arena host pointers
 * (globals/rodata — trusted, their extent is unknown here). Used to bound bulk
 * struct/array reads so a resolved edge-of-arena or retained dependency can
 * never read beyond its owned image. */
static inline size_t dkr_arena_room(const void *p) {
    uintptr_t up = (uintptr_t) p;
    uintptr_t base = (uintptr_t) g_dkrArenaBase;
    uintptr_t end = base + (uintptr_t) g_dkrArenaSize;
    size_t retained_room;
    if (up >= base && up < end) {
        return (size_t)(end - up);
    }
    if (dkr_replay_pass &&
        gfx_retained_task_dependency_room(p, &retained_room)) {
        return retained_room;
    }
    return (size_t)-1;   /* not arena-backed: trust it */
}

/*
 * STALE-TENANT GUARD for the shadow matrix registry.
 *
 * The registry keys by Mtx POINTER but copies the world/view-projection by
 * VALUE at registration time. An Mtx address is not a stable identity: the
 * game legitimately builds a different matrix in the same arena memory later in
 * the same frame, and three slot-2 pushers in camera.c do exactly that without
 * registering -- render_sprite_billboard's two pushes and
 * render_ortho_triangle_image's one. A plain lookup on those addresses returns
 * the FIRST tenant's binding, which describes a matrix that no longer exists
 * there.
 *
 * Measured on 2d697f6, level 40, MDKR_PRESENT_RATE=60: all 14 hard rejects were
 * dead tenants (`bytesmoved=1` on every line), 13 of them registered by
 * mtx_cam_push and 1 by mtx_head_push, and the worlds they served were track
 * geometry -- a 960-unit tile grid at a constant y = -43, plus tiles at
 * z = 3870/4515. Those worlds were not only recomposed against the wrong
 * matrix; they were fed to the shadow CASTER as the world of whatever billboard
 * had since taken the address. render_sprite_billboard's vehicle-part branch
 * (wheels, propellers, fans) does NOT enable billboard mode, so it takes the
 * caster path at full strength -- car wheels were casting shadows from track
 * tiles up to 3,107 world units away. That is the live visual defect, and it
 * exists whether or not anything is being interpolated.
 *
 * Comparing the registered image against the live bytes is what makes tenancy
 * checkable at all: a matching image means this binding still describes what is
 * at the key, and a mismatch is a MISS, exactly as if the address had never
 * been registered. A miss is always safe -- the caster skips the vertex and the
 * replay keeps the display list's own matrix.
 *
 * A binding without key_bytes_valid predates the tagging (only camera.c tags,
 * and it tags every registration) and is refused rather than trusted.
 */
static int dkr_shadow_tenancy_check = -1;

/*
 * MDKR_SHADOW_TENANCY=0 restores the pre-guard behaviour: serve the first
 * tenant's binding without checking that it still describes what is at the key.
 *
 * This is one half of the registered positive control, and it is a runtime
 * switch rather than a rebuild on purpose. The gate proves that production
 * content raises the stale-tenant counter and that this switch suppresses it;
 * MDKR_TEST_RECOMPOSE_REJECT independently proves the geometric tolerance's
 * rejection side now that owner-specific replay can make a stale world harmless.
 * Neither variable is set in production.
 */
static bool dkr_shadow_tenancy_enabled(void) {
    if (dkr_shadow_tenancy_check < 0) {
        const char *value = getenv("MDKR_SHADOW_TENANCY");
        dkr_shadow_tenancy_check = (value != NULL && value[0] == '0') ? 0 : 1;
    }
    return dkr_shadow_tenancy_check != 0;
}

static bool dkr_shadow_lookup_live(
    void *ma, GfxShadowMatrixBinding *out, bool *stale) {
    const void *identity = gfx_retained_task_original_address(ma);

    if (stale != NULL) {
        *stale = false;
    }
    if (!gfx_shadow_matrix_lookup(identity, out)) {
        return false;
    }
    if (!dkr_shadow_tenancy_enabled()) {
        return true;
    }
    if (!out->key_bytes_valid || ma == NULL ||
        dkr_arena_room(ma) < sizeof(Mtx)) {
        if (dkr_replay_pass && stale != NULL) {
            *stale = true;
        }
        return false;
    }
    if (memcmp(dkr_replay_pass && out->walked_key_bytes_valid
                   ? out->walked_key_bytes
                   : out->key_bytes,
               ma, sizeof(out->key_bytes)) != 0) {
        /*
         * CASTER EVIDENCE. Name the world this key would have handed to the
         * shadow caster, and the site that registered it. On level 40 these
         * come out as the 960-unit track-tile grid at y = -43 -- the worlds
         * that were being attributed to whatever billboard had taken the
         * address. Printed on the real walk as well as the replay, because the
         * caster consumed them on both.
         */
        if (present_perf_enabled()) {
            fprintf(stderr,
                    "[STALETENANT] frame=%d key=%p site=%d mobility=%d "
                    "deadworld3=[%.3f %.3f %.3f]\n",
                    dkr_frame_index, identity, out->site, (int)out->mobility,
                    out->world[3][0], out->world[3][1], out->world[3][2]);
        }
        dkr_shadow_stale_tenants++;
        if (stale != NULL) {
            *stale = true;
        }
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Color combiner → shader translation (ported from mgb64 gfx_generate_cc)   */
/* ------------------------------------------------------------------------- */

struct ColorCombiner {
    uint64_t cc_id;
    uint32_t cc_options;
    uint64_t shader_id0;
    uint32_t shader_id1;
    struct ShaderProgram *prg;
    uint8_t shader_input_mapping[2][7]; /* [color/alpha][input_idx] → G_CCMUX_* */
};

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 0xf) | ((b & 0xf) << 4) | ((c & 0x1f) << 8) | ((d & 7) << 13);
}
static inline uint32_t alpha_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 7) | ((b & 7) << 3) | ((c & 7) << 6) | ((d & 7) << 9);
}

static bool dkr_is_font_text_draw(void) {
    uint32_t expected_rgb =
        color_comb(G_CCMUX_ENVIRONMENT, G_CCMUX_TEXEL0,
                   G_CCMUX_ENV_ALPHA, G_CCMUX_TEXEL0);
    uint32_t expected_alpha =
        alpha_comb(G_ACMUX_TEXEL0, G_ACMUX_0,
                   G_ACMUX_PRIMITIVE, G_ACMUX_0);
    uint32_t first_cycle =
        expected_rgb | (expected_alpha << 16);

    return dkr_in_texrect &&
           ((uint32_t)rdp.combine_mode & 0x0fffffffu) == first_cycle;
}

static struct ShaderProgram *dkr_lookup_or_create_shader(uint64_t id0, uint32_t id1) {
    struct ShaderProgram *prg = gfx_rapi->lookup_shader(id0, id1);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(id0, id1);
        if (prg != NULL) {
            gfx_dkr_shader_programs_created++;
        }
        rendering_state.shader_program = prg;
    }
    return prg;
}

/* Translate a normalized combine cc_id + option flags into shader_id0/id1 and a
 * per-input source mapping. Faithful port of mgb64's gfx_generate_cc (Emill/PD
 * lineage), diagnostics removed. */
static void dkr_generate_cc(struct ColorCombiner *comb, uint64_t cc_id, uint32_t cc_options) {
    uint64_t shader_id0 = 0;
    uint32_t shader_id1 = cc_options;
    uint8_t  shader_input_mapping[2][7] = {{0}};
    bool is_2cyc    = (cc_options & SHADER_OPT_2CYC) != 0;
    bool want_alpha = (cc_options & SHADER_OPT_ALPHA) != 0;

    for (int i = 0; i < 2 && (i == 0 || is_2cyc); i++) { /* cycle */
        uint32_t rgb_a = (cc_id >> (i * 28)) & 0xf;
        uint32_t rgb_b = (cc_id >> (i * 28 + 4)) & 0xf;
        uint32_t rgb_c = (cc_id >> (i * 28 + 8)) & 0x1f;
        uint32_t rgb_d = (cc_id >> (i * 28 + 13)) & 7;
        uint32_t alp_a = (cc_id >> (i * 28 + 16)) & 7;
        uint32_t alp_b = (cc_id >> (i * 28 + 19)) & 7;
        uint32_t alp_c = (cc_id >> (i * 28 + 22)) & 7;
        uint32_t alp_d = (cc_id >> (i * 28 + 25)) & 7;

        /* Out-of-range CCMUX values produce ZERO on N64; sentinel them to fall
         * through to SHADER_0 rather than becoming COMBINED in cycle 1. */
        if (rgb_a >= 8) rgb_a = 0xFF;
        if (rgb_b >= 8) rgb_b = 0xFF;
        if (rgb_c >= 16) rgb_c = 0xFF;
        if (rgb_d == 7) rgb_d = 0xFF;

        /* (A-B)*0+D = D and (A-A)*C+D = D */
        if (rgb_a == rgb_b || rgb_c == 0) { rgb_a = rgb_b = rgb_c = 0; }
        if (alp_a == alp_b || alp_c == 0) { alp_a = alp_b = alp_c = 0; }

        uint32_t raw[2][4] = {
            { rgb_a, rgb_b, rgb_c, rgb_d },
            { alp_a, alp_b, alp_c, alp_d }
        };

        for (int j = 0; j < (want_alpha ? 2 : 1); j++) { /* 0=color, 1=alpha */
            static uint8_t input_number[2][32];
            static int next_input[2];
            if (i == 0) {
                memset(input_number[j], 0, sizeof(input_number[j]));
                next_input[j] = SHADER_INPUT_1;
            }
            for (int k = 0; k < 4; k++) {
                uint32_t vv = raw[j][k];
                int val = SHADER_0;
                if (j == 0) {
                    switch (vv) {
                        case G_CCMUX_TEXEL0: val = SHADER_TEXEL0; break;
                        case G_CCMUX_TEXEL1: val = SHADER_TEXEL1; break;
                        case G_CCMUX_TEXEL0_ALPHA: val = SHADER_TEXEL0A; break;
                        case G_CCMUX_TEXEL1_ALPHA: val = SHADER_TEXEL1A; break;
                        case 0: val = (i > 0) ? SHADER_COMBINED : SHADER_0; break;
                        case 6: val = SHADER_1; break; /* G_CCMUX_1 in C slot */
                        case G_CCMUX_NOISE: val = SHADER_NOISE; break;
                        case G_CCMUX_PRIMITIVE:
                        case G_CCMUX_SHADE:
                        case G_CCMUX_ENVIRONMENT:
                        case G_CCMUX_PRIMITIVE_ALPHA:
                        case G_CCMUX_SHADE_ALPHA:
                        case G_CCMUX_ENV_ALPHA:
                        case G_CCMUX_LOD_FRACTION:
                        case G_CCMUX_PRIM_LOD_FRAC:
                            if (input_number[j][vv] == 0) {
                                shader_input_mapping[j][next_input[j] - 1] = vv;
                                input_number[j][vv] = next_input[j]++;
                            }
                            val = input_number[j][vv];
                            break;
                        default: val = SHADER_0; break;
                    }
                } else {
                    switch (vv) {
                        case G_ACMUX_TEXEL0: val = SHADER_TEXEL0; break;
                        case G_ACMUX_TEXEL1: val = SHADER_TEXEL1; break;
                        case 0: /* COMBINED, or LOD_FRACTION in C slot */
                            if (k == 2) {
                                uint32_t key = G_CCMUX_LOD_FRACTION;
                                if (input_number[j][key] == 0) {
                                    shader_input_mapping[j][next_input[j] - 1] = key;
                                    input_number[j][key] = next_input[j]++;
                                }
                                val = input_number[j][key];
                            } else {
                                val = (i > 0) ? SHADER_COMBINED : SHADER_0;
                            }
                            break;
                        case 6: /* 1, or PRIM_LOD_FRAC in C slot */
                            if (k == 2) {
                                uint32_t key = G_CCMUX_PRIM_LOD_FRAC;
                                if (input_number[j][key] == 0) {
                                    shader_input_mapping[j][next_input[j] - 1] = key;
                                    input_number[j][key] = next_input[j]++;
                                }
                                val = input_number[j][key];
                            } else {
                                val = SHADER_1;
                            }
                            break;
                        case G_ACMUX_0: val = SHADER_0; break;
                        case G_ACMUX_PRIMITIVE:
                        case G_ACMUX_SHADE:
                        case G_ACMUX_ENVIRONMENT:
                            if (input_number[j][vv] == 0) {
                                shader_input_mapping[j][next_input[j] - 1] = vv;
                                input_number[j][vv] = next_input[j]++;
                            }
                            val = input_number[j][vv];
                            break;
                        default: val = SHADER_0; break;
                    }
                }
                shader_id0 |= (uint64_t)val << (i * 32 + j * 16 + k * 4);
            }
        }
    }

    if (is_2cyc) {
        bool c1_uses_combined = false;
        for (int j = 0; j < 2; j++)
            for (int k = 0; k < 4; k++)
                if (((shader_id0 >> (32 + j * 16 + k * 4)) & 0xf) == SHADER_COMBINED)
                    c1_uses_combined = true;
        if (!c1_uses_combined) shader_id0 &= ~(uint64_t)0xFFFFFFFF;
    }

    comb->cc_id = cc_id;
    comb->cc_options = cc_options;
    comb->shader_id0 = shader_id0;
    comb->shader_id1 = shader_id1;
    comb->prg = dkr_lookup_or_create_shader(shader_id0, shader_id1);
    memcpy(comb->shader_input_mapping, shader_input_mapping, sizeof(shader_input_mapping));
}

/* Small combiner cache (avoids re-deriving the input mapping each draw). */
#define DKR_CC_POOL_SIZE 256
static struct ColorCombiner cc_pool[DKR_CC_POOL_SIZE];
static int cc_pool_size;

static struct ColorCombiner *dkr_lookup_or_create_combiner(uint64_t cc_id, uint32_t cc_options) {
    for (int i = 0; i < cc_pool_size; i++) {
        if (cc_pool[i].cc_id == cc_id && cc_pool[i].cc_options == cc_options)
            return &cc_pool[i];
    }
    if (cc_pool_size == DKR_CC_POOL_SIZE) cc_pool_size = 0; /* regenerable; wrap */
    struct ColorCombiner *comb = &cc_pool[cc_pool_size++];
    dkr_generate_cc(comb, cc_id, cc_options);
    return comb;
}

/* ------------------------------------------------------------------------- */
/* Texture decode + upload                                                   */
/* ------------------------------------------------------------------------- */

static uint8_t *tex_decode_buf;
static size_t   tex_decode_cap;
static uint8_t *font_sdf_buf;
static size_t   font_sdf_cap;

static bool ensure_decode_buf(size_t bytes) {
    if (bytes <= tex_decode_cap) return tex_decode_buf != NULL;
    size_t cap = tex_decode_cap ? tex_decode_cap : 4096;
    while (cap < bytes) cap *= 2;
    uint8_t *nb = (uint8_t *)realloc(tex_decode_buf, cap);
    if (!nb) return false;
    tex_decode_buf = nb;
    tex_decode_cap = cap;
    return true;
}

static bool ensure_font_sdf_buf(size_t bytes) {
    uint8_t *grown;

    if (bytes <= font_sdf_cap) {
        return font_sdf_buf != NULL;
    }
    grown = (uint8_t *)realloc(font_sdf_buf, bytes);
    if (grown == NULL) {
        return false;
    }
    font_sdf_buf = grown;
    font_sdf_cap = bytes;
    return true;
}

/* Texels per row given a source row pitch (bytes) and pixel size. */
static uint32_t texels_per_row(uint32_t line_bytes, uint8_t siz) {
    switch (siz) {
        case G_IM_SIZ_4b:  return line_bytes * 2;
        case G_IM_SIZ_8b:  return line_bytes;
        case G_IM_SIZ_16b: return line_bytes / 2;
        case G_IM_SIZ_32b: return line_bytes / 4;
        default:           return line_bytes;
    }
}

static inline void palette_to_rgba32(uint16_t entry, uint8_t *out) {
    gfx_palette_entry_to_rgba32(entry, rdp.palette_fmt == G_TT_IA16, out);
}

/* ---- "Interlaced" textures: the odd-row TMEM word swap ------------------- *
 * Real RDP TMEM stores every ODD texture row with the two 32-bit halves of each
 * 64-bit word exchanged, and the texture-fetch unit un-exchanges them on the way
 * out. LOADBLOCK normally performs that exchange itself, driven by the `dxt`
 * line-advance rate, so DRAM holds a plain linear image and the two swaps cancel.
 *
 * `dxt` is a 1.11 reciprocal (CALC_DXT = ceil(2048 / words_per_line)), so it can
 * only express a row length exactly when the row is a power-of-two number of
 * 64-bit words. For every other width the accumulator drifts and rows get
 * mis-swapped, so libultra offers the `...S` macro family — "the S at the end
 * means odd lines are already word Swapped" (PR/gbi.h:2699) — which passes
 * dxt = 0 (no load-time exchange) and relies on the ASSET being pre-swizzled to
 * cancel the fetch-time exchange instead. DKR's asset tool does exactly that and
 * records it as TextureHeader.flags bit 0x04 (RENDER_LINE_SWAP, "interlaced");
 * material_init() then picks the S macros for those textures
 * (game/src/textures_sprites.c:1631).
 *
 * We have no TMEM: dkr_upload_tile_texture reads DRAM rows straight through, so
 * the fetch-time exchange never happens for us and a pre-swizzled asset decodes
 * with every odd row's texel groups transposed — "mostly right, but scrambled".
 * So undo the swizzle here, for dxt == 0 loads only.
 *
 * Chunk size: a TMEM word spans 8 / LINE_BYTES texels, and the source row pitch
 * is `line_bytes` (already doubled for 32b, see dkr_upload_tile_texture), so the
 * chunk is 8 source bytes for 4b/8b/16b and 16 for 32b — i.e. 16/8/4/4 texels
 * respectively. */
static uint8_t *tex_row_buf;
static size_t   tex_row_cap;

static bool ensure_row_buf(size_t bytes) {
    if (bytes <= tex_row_cap) return tex_row_buf != NULL;
    size_t cap = tex_row_cap ? tex_row_cap : 256;
    while (cap < bytes) cap *= 2;
    uint8_t *nb = (uint8_t *)realloc(tex_row_buf, cap);
    if (!nb) return false;
    tex_row_buf = nb;
    tex_row_cap = cap;
    return true;
}

/* Copy one source row into `dst`, exchanging the two halves of every `chunk`
 * bytes. A trailing partial chunk is copied verbatim (LOADBLOCK pitches are
 * always whole chunks, but never read past the row we were given). */
static void unswap_row(uint8_t *dst, const uint8_t *src, uint32_t len, uint32_t chunk) {
    uint32_t half = chunk / 2;
    uint32_t i = 0;
    for (; i + chunk <= len; i += chunk) {
        memcpy(dst + i, src + i + half, half);
        memcpy(dst + i + half, src + i, half);
    }
    if (i < len) memcpy(dst + i, src + i, len - i);
}

/* Number of texture uploads that took the un-swizzle path above. Exposed so a
 * regression check can confirm the route it drives actually reaches it. */
uint32_t gfx_dkr_texload_line_swapped;

/* Final decoded depth state at triangle emission. These are process-lifetime
 * counters, like the texture-path telemetry above, and are reported when a
 * headless run exits. */
uint64_t gfx_dkr_depth_compared_triangles;
uint64_t gfx_dkr_decal_triangles;

/* MDKR_LINESWAP=off (or =0) reproduces the pre-fix decode, for A/B measurement —
 * same convention as MDKR_NEARCLIP=off and MDKR_VI_PACE=off.
 * tests/check_texture_lineswap.py runs the route BOTH ways and requires the
 * artifact metric to separate, so this hook is the check's own positive control:
 * it cannot pass vacuously.
 *
 * Both spellings are accepted on purpose. MDKR_AUDIO tests only for the digit '0',
 * so `MDKR_AUDIO=off` is a silent no-op — that trap is documented in CONTRIBUTING
 * and is not worth reproducing. "on", "1" and unset all leave the fix enabled;
 * note a bare `s[0] == 'o'` test would have read "on" as "off". */
static int dkr_lineswap_mode = -1;
static bool dkr_lineswap_on(void) {
    if (dkr_lineswap_mode < 0) {
        const char *s = getenv("MDKR_LINESWAP");
        bool off = s && (s[0] == '0' ||
                         ((s[0] == 'o' || s[0] == 'O') && (s[1] == 'f' || s[1] == 'F')));
        dkr_lineswap_mode = off ? 0 : 1;
    }
    return dkr_lineswap_mode != 0;
}

/* Decode the render tile's texture into RGBA32 and upload it. Standard N64 tile
 * model: source row pitch = tile.line_size_bytes, dimensions from SETTILESIZE
 * (falling back to the loaded block size). */
/* Scratch for mip levels 1..N-1; grows to the largest texture seen. */
static uint8_t *tex_mip_buf;
static size_t tex_mip_cap;

static bool ensure_mip_buf(size_t need) {
    if (need <= tex_mip_cap) {
        return tex_mip_buf != NULL || need == 0;
    }
    {
        uint8_t *grown = (uint8_t *) realloc(tex_mip_buf, need);
        if (grown == NULL) {
            return false;
        }
        tex_mip_buf = grown;
        tex_mip_cap = need;
    }
    return true;
}

/* Resolve the byte pitch consumed by the software decoder. Keeping this in one
 * helper is load-bearing: the texture-cache identity must use the exact same
 * pitch as the upload path or a tile reinterpretation can bind old pixels. */
static uint32_t dkr_tile_source_line_bytes(uint8_t td, uint32_t source_size_bytes) {
    uint32_t line_bytes = rdp.tile[td].line_size_bytes;

    /* F3DDKR / N64 32-bit tile LINE quirk (see PR/gbi.h: G_IM_SIZ_32b_LINE_BYTES
     * == 2, not 4). gDPLoadTextureBlock derives the render tile's `line` field
     * counting only 2 bytes per 32-bit texel, because on real RDP a 32-bit
     * texel's RG and BA halves are split across two TMEM banks so a TMEM row is
     * width*2 bytes. Our source, however, is plain contiguous RGBA32 in the arena
     * (4 bytes/texel, tightly packed by LoadBlock), so the true SOURCE row pitch
     * is DOUBLE the tile line. Without this, every 32-bit texture — the RGBA32
     * menu font atlas and the DKR-logo TEXRECT strips — decodes at half stride:
     * successive rows overlap and read adjacent memory, collapsing glyphs to
     * solid blocks and the logo to vertical bars. (16/8/4-bit LINE_BYTES already
     * equal the real bytes/texel, so only 32-bit needs the correction.) */
    if (rdp.tile[td].siz == G_IM_SIZ_32b) line_bytes *= 2;
    if (line_bytes == 0) {
        /* No explicit pitch: assume the whole loaded block is one row. */
        line_bytes = source_size_bytes;
    }
    return line_bytes;
}

static bool dkr_upload_tile_texture(uint8_t td, bool cutout,
                                    uint32_t *out_w, uint32_t *out_h) {
    if (td >= 8) return false;
    uint8_t fmt = rdp.tile[td].fmt;
    uint8_t siz = rdp.tile[td].siz;
    uint32_t tmem = rdp.tile[td].tmem;
    if (tmem >= 512) tmem = 0;
    const uint8_t *src = rdp.loaded_texture[tmem].addr;
    if (!src) return false;
    const uint32_t source_size_bytes = rdp.loaded_texture[tmem].size_bytes;
    const uint32_t line_bytes =
        dkr_tile_source_line_bytes(td, source_size_bytes);

    uint32_t width  = rdp.tile[td].width;
    uint32_t height = rdp.tile[td].height;
    if (width == 0)  width  = texels_per_row(line_bytes, siz);
    if (height == 0 && line_bytes) height = source_size_bytes / line_bytes;
    if (width == 0 || height == 0) return false;
    if (width > 1024) width = 1024;
    if (height > 1024) height = 1024;

    /* Never sample past the arena: clamp rows to the bytes actually available
     * from `src` (a mis-decoded tile size or an edge pointer could otherwise run
     * the decode loop into the unmapped page past the 16 MB arena). */
    if (line_bytes > 0) {
        size_t room = dkr_arena_room(src);
        if (room != (size_t)-1) {
            uint32_t max_rows = (uint32_t)(room / line_bytes);
            if (height > max_rows) height = max_rows;
            /* The row clamp only bounds whole pitches. A tile whose width needs
             * more bytes than its own pitch (width * bpp > line_bytes) reads
             * past the end of the LAST row, which is the arena edge; drop the
             * rows whose full width is not backed. */
            if (width > 0) {
                uint32_t row_bytes;   /* inverse of texels_per_row() */
                switch (siz) {
                    case G_IM_SIZ_4b:  row_bytes = (width + 1u) / 2u; break;
                    case G_IM_SIZ_16b: row_bytes = width * 2u; break;
                    case G_IM_SIZ_32b: row_bytes = width * 4u; break;
                    default:           row_bytes = width; break;
                }
                while (height > 0 &&
                       (size_t)(height - 1u) * line_bytes + row_bytes > room) {
                    height--;
                }
            }
        }
    }
    if (width == 0 || height == 0) return false;

    if (!ensure_decode_buf((size_t)width * height * 4)) return false;
    uint8_t *dst = tex_decode_buf;

    /* Pre-swizzled ("interlaced") source: un-swap odd rows. See the block comment
     * above unswap_row(). A 1-row texture has no odd row, so nothing to do. */
    bool line_swapped = rdp.loaded_texture[tmem].line_swapped && height > 1;
    const uint32_t swap_chunk = (siz == G_IM_SIZ_32b) ? 16u : 8u;
    if (line_swapped) {
        gfx_dkr_texload_line_swapped++;   /* counted even when the A/B hook is off,
                                           * so the check can prove route coverage
                                           * from either arm of the comparison */
        if (!dkr_lineswap_on()) line_swapped = false;
        else if (!ensure_row_buf(line_bytes)) return false;
    }

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = src + (size_t)y * line_bytes;
        if (line_swapped && (y & 1)) {
            unswap_row(tex_row_buf, row, line_bytes, swap_chunk);
            row = tex_row_buf;
        }
        uint8_t *o = dst + (size_t)y * width * 4;
        for (uint32_t x = 0; x < width; x++, o += 4) {
            if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b) {
                /* Texels are big-endian ROM bytes (swap_texture leaves them
                 * untouched by design); read the 16-bit value MSB-first. */
                uint16_t c = ((uint16_t) row[x * 2] << 8) | row[x * 2 + 1];
                uint8_t r = c >> 11, g = (c >> 6) & 0x1f, b = (c >> 1) & 0x1f;
                o[0] = SCALE_5_8(r); o[1] = SCALE_5_8(g); o[2] = SCALE_5_8(b);
                o[3] = (c & 1) ? 255 : 0;
            } else if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_32b) {
                const uint8_t *p = row + x * 4;
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3];
            } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_16b) {
                /* Texels are big-endian ROM bytes (swap_texture leaves them
                 * untouched by design); read the 16-bit value MSB-first. */
                uint16_t c = ((uint16_t) row[x * 2] << 8) | row[x * 2 + 1];
                uint8_t iv = c >> 8;
                o[0] = o[1] = o[2] = iv; o[3] = c & 0xff;
            } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_8b) {
                uint8_t b = row[x];
                uint8_t iv = (b >> 4) & 0xf, a = b & 0xf;
                o[0] = o[1] = o[2] = SCALE_4_8(iv); o[3] = SCALE_4_8(a);
            } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_4b) {
                uint8_t b = row[x >> 1];
                uint8_t part = (x & 1) ? (b & 0xf) : ((b >> 4) & 0xf);
                uint8_t iv = part >> 1, a = part & 1;
                o[0] = o[1] = o[2] = SCALE_3_8(iv); o[3] = a ? 255 : 0;
            } else if (fmt == G_IM_FMT_I && siz == G_IM_SIZ_8b) {
                uint8_t iv = row[x];
                o[0] = o[1] = o[2] = iv; o[3] = iv;
            } else if (fmt == G_IM_FMT_I && siz == G_IM_SIZ_4b) {
                uint8_t b = row[x >> 1];
                uint8_t part = (x & 1) ? (b & 0xf) : ((b >> 4) & 0xf);
                uint8_t iv = SCALE_4_8(part);
                o[0] = o[1] = o[2] = iv; o[3] = iv;
            } else if (fmt == G_IM_FMT_CI && siz == G_IM_SIZ_8b) {
                palette_to_rgba32(rdp.palette[row[x]], o);
            } else if (fmt == G_IM_FMT_CI && siz == G_IM_SIZ_4b) {
                uint8_t b = row[x >> 1];
                uint8_t idx = (x & 1) ? (b & 0xf) : ((b >> 4) & 0xf);
                const uint16_t *pal = rdp.palette + ((rdp.tile[td].palette & 0xf) * 16);
                palette_to_rgba32(pal[idx], o);
            } else {
                /* Unknown format: opaque magenta so it is visually obvious. */
                o[0] = 255; o[1] = 0; o[2] = 255; o[3] = 255;
            }
        }
    }

    /*
     * Derive high-resolution coverage only for atlases registered at the
     * ASSET_FONTS load boundary, and only in Remastered. The source atlas
     * remains the cache identity; no derived bytes leave this process.
     *
     * Region-isolated construction prevents adjacent packed glyphs from
     * influencing one another. out_w/out_h deliberately remain the logical N64
     * atlas size: the existing texel coordinates then address the same glyph
     * rectangles in a GPU image whose physical dimensions are four times larger.
     */
    {
        const GfxFontRegistryEntry *font_entry =
            gfx_font_registry_find(
                &dkr_font_registry,
                gfx_retained_task_original_address(src));
        bool remaster_font =
            g_pcRemasterFX && dkr_font_sdf_enabled() &&
            dkr_is_font_text_draw() &&
            font_entry != NULL && font_entry->region_count != 0;
        if (remaster_font) {
            size_t output_bytes =
                gfx_font_sdf_output_bytes(width, height, DKR_FONT_UPSCALE);
            if (output_bytes != 0 &&
                ensure_font_sdf_buf(output_bytes) &&
                gfx_font_sdf_upscale_rgba(
                    dst, width, height, DKR_FONT_UPSCALE,
                    font_entry->regions, font_entry->region_count,
                    font_sdf_buf, font_sdf_cap) &&
                gfx_rapi->upload_texture(
                    font_sdf_buf,
                    (int)(width * DKR_FONT_UPSCALE),
                    (int)(height * DKR_FONT_UPSCALE))) {
                gfx_dkr_font_sdf_uploads++;
                *out_w = width;
                *out_h = height;
                return true;
            }
            /* A resource failure falls through to the faithful source atlas. */
        }
    }

    /*
     * Mip chain. Built here, once, at texture-cache fill — not per backend and
     * not by the driver. glGenerateMipmap is never called, so the silent NPOT
     * failure on macOS Metal that caused mipmaps to be switched off in the first
     * place (gfx_opengl.c:1744) cannot recur.
     *
     * `cutout` comes from the RDP's CVG_X_ALPHA state at the draw that triggered
     * this fill: alpha-tested materials get coverage-preserving reduction so
     * fences and foliage do not erode with distance.
     */
    if (g_pcMipmaps && gfx_rapi->upload_texture_mipped != NULL &&
        gfx_mip_level_count(width, height) > 1) {
        size_t need = gfx_mip_chain_bytes(width, height);
        if (ensure_mip_buf(need)) {
            GfxMipChain chain;
            bool built = cutout
                ? gfx_mip_build_cutout(dst, width, height, tex_mip_buf, need,
                                       GFX_TEXTURE_EDGE_ALPHA_THRESHOLD_U8, &chain)
                : gfx_mip_build(dst, width, height, tex_mip_buf, need, &chain);
            if (built &&
                gfx_rapi->upload_texture_mipped(chain.level, chain.width,
                                                chain.height, chain.level_count)) {
                gfx_dkr_mipmapped_uploads++;
                gfx_dkr_mip_levels_uploaded += (uint64_t) chain.level_count;
                *out_w = width;
                *out_h = height;
                return true;
            }
        }
        /* Any failure falls through to the single-level path rather than
         * leaving the texture half-uploaded. */
    }

    if (!gfx_rapi->upload_texture(dst, width, height)) return false;
    *out_w = width;
    *out_h = height;
    return true;
}

/* Every non-content value that can change decoded or uploaded bytes belongs in
 * the key. Source-content lifetime is enforced separately by allocator/font
 * invalidation above (and audited by MDKR_TEXCACHE_VERIFY). Source pitch/span
 * affect row addressing and inferred height, while mip/cutout policy changes
 * the uploaded mip chain without changing level zero. Keeping them explicit
 * avoids first-use-wins textures. */
#define DKR_TEXCACHE_SIZE 1024
struct DkrTexCacheEntry {
    struct DkrTexCacheKey key;
    uint32_t texture_id;
    uint32_t upload_w, upload_h;
    uint32_t src_hash;    /* content hash at upload time (verify mode only) */
    bool valid;
};
static struct DkrTexCacheEntry tex_cache[DKR_TEXCACHE_SIZE];
static int tex_cache_next;

static void dkr_forget_texture_binding(uint32_t texture_id) {
    for (int unit = 0; unit < 2; unit++) {
        if (rendering_state.bound_texture_id[unit] == texture_id) {
            rendering_state.bound_texture_id[unit] = 0;
            rendering_state.bound_texture_linear[unit] = false;
            rendering_state.bound_texture_cms[unit] = 0;
            rendering_state.bound_texture_cmt[unit] = 0;
            rendering_state.bound_texture_lod0[unit] = false;
        }
    }
}

static void dkr_texcache_delete_slot(int slot) {
    uint32_t texture_id;

    if (slot < 0 || slot >= DKR_TEXCACHE_SIZE) {
        return;
    }
    texture_id = tex_cache[slot].texture_id;
    if (texture_id == 0) {
        memset(&tex_cache[slot], 0, sizeof(tex_cache[slot]));
        return;
    }
    dkr_forget_texture_binding(texture_id);
    if (gfx_rapi != NULL && gfx_rapi->delete_texture != NULL) {
        gfx_rapi->delete_texture(texture_id);
        gfx_dkr_texture_ids_deleted++;
        if (gfx_dkr_texture_ids_live > 0) {
            gfx_dkr_texture_ids_live--;
        }
    }
    memset(&tex_cache[slot], 0, sizeof(tex_cache[slot]));
}

static void dkr_texture_id_acquired(uint32_t texture_id) {
    if (texture_id == 0) {
        return;
    }
    gfx_dkr_texture_ids_created++;
    gfx_dkr_texture_ids_live++;
    if (gfx_dkr_texture_ids_live > gfx_dkr_texture_ids_high_water) {
        gfx_dkr_texture_ids_high_water = gfx_dkr_texture_ids_live;
    }
    if (dkr_resource_generation.active &&
        gfx_dkr_texture_ids_live >
            dkr_resource_generation.texture_live_peak) {
        dkr_resource_generation.texture_live_peak =
            gfx_dkr_texture_ids_live;
    }
}

/* ---- Texture-cache invalidation on arena reuse -------------------------- *
 * The cache key is the SOURCE ADDRESS (plus fmt/siz/dims/palette), so an entry
 * survives the game freeing that memory and mempool handing the same bytes to a
 * different asset. When the new asset has the same dimensions and format as the
 * old occupant the lookup then HITS and binds the previous asset's GPU texture —
 * a silent wrong-image bug with no crash. DKR's menus hit this constantly
 * because every screen frees the previous screen's assets and reloads into the
 * same pool (all ten TRACK SELECT world backgrounds are 64x32 RGBA16, so they
 * alias each other perfectly).
 *
 * Fix: the game's allocator tells us when arena bytes are recycled
 * (mempool_slot_clear -> here), and every entry overlapping that span is
 * dropped. Steady-state cost is one pass over the cache per free; no hashing.
 * MDKR_TEXCACHE_VERIFY=1 turns on the content check that found this (it hashes
 * the source on every bind and reports any hit whose content changed), so the
 * bug cannot silently come back. */
void gfx_dkr_texcache_invalidate_range(const void *base, uint32_t size) {
    const uint8_t *lo = (const uint8_t *)base;
    const uint8_t *hi = lo + size;
    if (!lo || size == 0) return;
    for (int i = 0; i < DKR_TEXCACHE_SIZE; i++) {
        if (!tex_cache[i].valid) continue;
        /* Overlap test against the span the entry was uploaded from, so an entry
         * whose texels merely START before `base` but reach into it is dropped
         * too. */
        const uint8_t *elo = tex_cache[i].key.addr;
        const uint32_t source_size_bytes =
            tex_cache[i].key.source_size_bytes;
        const uint8_t *ehi =
            elo + (source_size_bytes ? source_size_bytes : 1);
        if (elo < hi && lo < ehi) {
            dkr_texcache_delete_slot(i);
        }
    }
}

void gfx_dkr_resource_generation_begin(
    int32_t level, int32_t players, int32_t cutscene) {
    /*
     * A stage boundary frees and reissues the memory the last walked display
     * list points into. Retiring it here is the same rule the shadow static
     * cache follows: presentation history never crosses two unrelated scenes.
     */
    gfx_dkr_replay_invalidate();
    /* Test-only seam for check_shadow_stage_reset.py's positive control:
     * suppressing the reset restores the historical diagnostic-gated defect
     * so the gate can prove it would detect a regression. */
    static int s_skip_stage_reset = -1;
    if (s_skip_stage_reset < 0) {
        const char *value = getenv("MDKR_TEST_SHADOW_STAGE_RESET_SKIP");
        s_skip_stage_reset =
            value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    }
    dkr_shadow_stage_generation++;
    if (dkr_shadow_stage_generation == 0) {
        dkr_shadow_stage_generation = 1;
    }
    if (!s_skip_stage_reset) {
        gfx_shadow_stage_begin(dkr_shadow_stage_generation);
    }
    if (!mdkr_resource_trace_enabled()) {
        return;
    }
    if (dkr_resource_generation.active) {
        mdkr_trace(
            "renderer_generation: level=%d players=%d cutscene=%d "
            "texStart=%u texCreated=%llu texDeleted=%llu texLive=%u "
            "texPeak=%u shaderCreates=%llu",
            (int)dkr_resource_generation.level,
            (int)dkr_resource_generation.players,
            (int)dkr_resource_generation.cutscene,
            (unsigned)dkr_resource_generation.texture_live_start,
            (unsigned long long)(
                gfx_dkr_texture_ids_created -
                dkr_resource_generation.texture_created_start),
            (unsigned long long)(
                gfx_dkr_texture_ids_deleted -
                dkr_resource_generation.texture_deleted_start),
            (unsigned)gfx_dkr_texture_ids_live,
            (unsigned)dkr_resource_generation.texture_live_peak,
            (unsigned long long)(
                gfx_dkr_shader_programs_created -
                dkr_resource_generation.shader_created_start));
    }
    dkr_resource_generation.active = true;
    dkr_resource_generation.level = level;
    dkr_resource_generation.players = players;
    dkr_resource_generation.cutscene = cutscene;
    dkr_resource_generation.texture_created_start =
        gfx_dkr_texture_ids_created;
    dkr_resource_generation.texture_deleted_start =
        gfx_dkr_texture_ids_deleted;
    dkr_resource_generation.shader_created_start =
        gfx_dkr_shader_programs_created;
    dkr_resource_generation.texture_live_start =
        gfx_dkr_texture_ids_live;
    dkr_resource_generation.texture_live_peak =
        gfx_dkr_texture_ids_live;
    mdkr_trace(
        "registry_state: level=%d players=%d cutscene=%d "
        "live=%u high=%u ambiguous=%u fullFails=%u maxProbe=%u",
        (int)level, (int)players, (int)cutscene, (unsigned)gfx_ptr_live,
        (unsigned)gfx_ptr_high_water, (unsigned)gfx_ptr_ambiguous,
        (unsigned)gfx_ptr_full_fails, (unsigned)gfx_ptr_max_probe);
}

void gfx_dkr_stage_resources_released(void) {
    gfx_ptr_clear_transient();
}

static int dkr_texcache_verify = -1;
static inline bool dkr_texcache_verify_on(void) {
    if (dkr_texcache_verify < 0) {
        const char *e = getenv("MDKR_TEXCACHE_VERIFY");
        dkr_texcache_verify = (e && e[0] == '1') ? 1 : 0;
    }
    return dkr_texcache_verify != 0;
}
/* Number of cache hits whose source content had changed since upload — i.e.
 * frames that rendered a stale texture. Must stay 0. */
uint32_t gfx_dkr_texcache_stale_hits;

static uint32_t dkr_src_hash(const uint8_t *addr, uint32_t size) {
    size_t room = dkr_arena_room(addr);
    if (room != (size_t)-1 && size > room) size = (uint32_t)room;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < size; i++) h = (h ^ addr[i]) * 16777619u;
    return h;
}

static uint32_t dkr_palette_hash(uint8_t fmt) {
    if (fmt != G_IM_FMT_CI) return 0;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 256; i++) { h = (h ^ rdp.palette[i]) * 16777619u; }
    return h;
}

/* The same TLUT words decode to different pixels under G_TT_RGBA16 and
 * G_TT_IA16 (dkr_palette_entry_to_rgba32), so the type is part of the decode
 * input. Zero outside CI so a TEXTLUT mode change cannot split non-paletted
 * cache entries. */
static uint32_t dkr_palette_fmt_key(uint8_t fmt) {
    return fmt == G_IM_FMT_CI ? rdp.palette_fmt : 0u;
}

/* Bind the render tile's texture to a sampler unit; import+cache on miss.
 * Returns the uploaded dimensions (for UV normalization). */
static bool dkr_bind_tile(int unit, uint8_t td, bool cutout, uint32_t *w, uint32_t *h) {
    if (td >= 8) return false;
    uint32_t tmem = rdp.tile[td].tmem < 512 ? rdp.tile[td].tmem : 0;
    const uint8_t *addr = rdp.loaded_texture[tmem].addr;
    const uint8_t *source_identity;
    if (!addr) return false;
    source_identity = (const uint8_t *)
        gfx_retained_task_original_address(addr);
    uint8_t fmt = rdp.tile[td].fmt, siz = rdp.tile[td].siz, pal = rdp.tile[td].palette;
    uint16_t tw = rdp.tile[td].width, th = rdp.tile[td].height;
    uint32_t ph = dkr_palette_hash(fmt);
    uint32_t pf = dkr_palette_fmt_key(fmt);
    const uint32_t source_size_bytes = rdp.loaded_texture[tmem].size_bytes;
    const uint32_t source_line_bytes =
        dkr_tile_source_line_bytes(td, source_size_bytes);
    bool lsw = rdp.loaded_texture[tmem].line_swapped;
    const GfxFontRegistryEntry *font_entry =
        gfx_font_registry_find(&dkr_font_registry, source_identity);
    bool font_remastered =
        g_pcRemasterFX && dkr_font_sdf_enabled() &&
        dkr_is_font_text_draw() &&
        font_entry != NULL && font_entry->region_count != 0;
    const struct DkrTexCacheKey key = {
        .addr = source_identity,
        .source_line_bytes = source_line_bytes,
        .source_size_bytes = source_size_bytes,
        .palette_hash = ph,
        .palette_fmt = pf,
        .width = tw,
        .height = th,
        .fmt = fmt,
        .siz = siz,
        .palette = pal,
        .line_swapped = lsw,
        .font_remastered = font_remastered,
        .mipmaps = g_pcMipmaps && gfx_rapi != NULL &&
            gfx_rapi->upload_texture_mipped != NULL,
        .cutout = cutout,
    };

    int hit = -1;
    for (int i = 0; i < DKR_TEXCACHE_SIZE; i++) {
        if (tex_cache[i].valid &&
            dkr_texcache_key_equal(&tex_cache[i].key, &key)) {
            hit = i; break;
        }
    }
    const bool verify = dkr_texcache_verify_on();
    const uint32_t now_hash =
        verify ? dkr_src_hash(addr, source_size_bytes) : 0;
    const bool was_hit = (hit >= 0);
    if (hit < 0) {
        int slot = tex_cache_next;
        bool acquired = !tex_cache[slot].valid;
        tex_cache_next = (tex_cache_next + 1) % DKR_TEXCACHE_SIZE;
        uint32_t tid = acquired ? gfx_rapi->new_texture()
                                : tex_cache[slot].texture_id;
        if (tid == 0) {
            return false;
        }
        if (acquired) {
            dkr_texture_id_acquired(tid);
        } else {
            /* The backend object survives cache-slot replacement, but its
             * decoded pixels and mip layout are about to change. Forget every
             * frontend sampler memo that names this ID so the first draw of the
             * replacement republishes filtering, wrap and LOD policy. */
            dkr_forget_texture_binding(tid);
        }
        gfx_rapi->select_texture(unit, tid);
        uint32_t uw = 0, uh = 0;
        if (!dkr_upload_tile_texture(td, cutout, &uw, &uh)) {
            /*
             * A failed upload invalidates both a new handle and a reused cache
             * handle: the backend object may now contain partial/new pixels,
             * so retaining the old metadata would turn the next lookup into a
             * false cache hit.
             */
            if (acquired) {
                /* The slot carries no id until the entry below is written, so
                 * delete_slot cannot reach a handle acquired this call; seat it
                 * first or the texture and its live-count leak. */
                tex_cache[slot].texture_id = tid;
            }
            dkr_texcache_delete_slot(slot);
            return false;
        }
        tex_cache[slot] = (struct DkrTexCacheEntry){
            .key = key,
            .texture_id = tid, .upload_w = uw, .upload_h = uh,
            .src_hash = now_hash, .valid = true };
        hit = slot;
    } else {
        gfx_rapi->select_texture(unit, tex_cache[hit].texture_id);
        if (verify && tex_cache[hit].src_hash != now_hash) {
            gfx_dkr_texcache_stale_hits++;
            if (gfx_dkr_texcache_stale_hits <= 20) {
                fprintf(stderr,
                        "[TEXCACHE] STALE HIT f=%d slot=%d addr=%p %ux%u fmt=%u siz=%u "
                        "uploadedHash=%08x nowHash=%08x\n",
                        dkr_frame_index, hit, (const void *)addr, tw, th, fmt, siz,
                        tex_cache[hit].src_hash, now_hash);
            }
        }
    }
    if (dkr_trace_this_frame) {
        DTRACE("  bind unit=%d %s slot=%d addr=%p %ux%u fmt=%u siz=%u tid=%u sz=%u", unit,
               was_hit ? "HIT " : "MISS", hit, (const void *)addr, tw, th, fmt, siz,
               tex_cache[hit].texture_id, source_size_bytes);
    }
    const uint32_t texture_id = tex_cache[hit].texture_id;
    const bool texture_changed = rendering_state.bound_texture_id[unit] != texture_id;
    *w = tex_cache[hit].upload_w;
    *h = tex_cache[hit].upload_h;

    /* Sampler wrap/filter from tile clamp/mirror bits + othermode filter. */
    bool linear = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
    /*
     * A font atlas packs glyph cells directly beside one another. Sampling the
     * 4x prefiltered coverage with point filtering retains its subpixel contour
     * without allowing a hardware bilinear tap to cross into the next cell.
     */
    if (font_remastered) {
        linear = false;
    }
    /*
     * Screen-space 2D takes level 0 only. Mipmapping the HUD, menus and text
     * would blur them and move the pixels the oracle scores, and a rect drawn
     * at 1:1 has no business sampling a reduced level. The chain is still built
     * and uploaded — the same texture can be used by 3D geometry elsewhere —
     * so this is enforced at the SAMPLER rather than by withholding the mips.
     */
    bool lod0 = dkr_in_texrect || font_remastered;
    uint32_t cms =
        font_remastered ? G_TX_CLAMP : rdp.tile[td].cms;
    uint32_t cmt =
        font_remastered ? G_TX_CLAMP : rdp.tile[td].cmt;
    if (texture_changed ||
        rendering_state.bound_texture_linear[unit] != linear ||
        rendering_state.bound_texture_cms[unit] != cms ||
        rendering_state.bound_texture_cmt[unit] != cmt ||
        rendering_state.bound_texture_lod0[unit] != lod0) {
        g_gfxSamplerLod0Only = lod0 ? 1 : 0;
        gfx_rapi->set_sampler_parameters(unit, linear, cms, cmt);
        g_gfxSamplerLod0Only = 0;
        rendering_state.bound_texture_linear[unit] = linear;
        rendering_state.bound_texture_cms[unit] = cms;
        rendering_state.bound_texture_cmt[unit] = cmt;
        rendering_state.bound_texture_lod0[unit] = lod0;
    }
    /* OpenGL stores wrap/filter parameters on each texture object. A newly
     * bound texture therefore needs its own sampler state even when the
     * requested values match the previous binding. WebGPU's separate sampler
     * objects tolerate the same refresh. Publish the binding only after that
     * decision so texture identity remains part of the memo key. */
    rendering_state.bound_texture_id[unit] = texture_id;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Flush                                                                     */
/* ------------------------------------------------------------------------- */

static void gfx_flush(void) {
    if (buf_vbo_len > 0) {
        gfx_rapi->draw_triangles(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        buf_vbo_len = 0;
        buf_vbo_num_tris = 0;
    }
}

static void dkr_prepare_draw_target(void);
static void dkr_begin_primitive(bool overlay_candidate) {
    dkr_primitive_serial++;
    dkr_primitive_prepared = false;
    dkr_primitive_overlay_candidate = overlay_candidate;
}

/* ------------------------------------------------------------------------- */
/* Per-batch draw state (shader / textures / blend / depth / viewport)       */
/* ------------------------------------------------------------------------- */

static struct {
    struct CCFeatures feat;
    struct ColorCombiner *comb;
    bool  use_texture;
    bool  use_fog;
    uint8_t tile_base;
    uint32_t tex_w[2], tex_h[2];
    int   num_inputs;
} cur;

static int dkr_rdp_gradient_legacy = -1;
static int dkr_texedge_legacy = -1;
static int dkr_rl1_arm = -1;
static int dkr_rl5_enabled = -1;

static bool dkr_texedge_legacy_enabled(void) {
    if (dkr_texedge_legacy < 0) {
        const char *value = getenv("MDKR_TEXEDGE");
        dkr_texedge_legacy = value != NULL && strcmp(value, "legacy") == 0;
    }
    return dkr_texedge_legacy != 0;
}

/*
 * Classify an RDP blender word as a hardware cutout (alpha test) rather than a
 * translucent blend.
 *
 * On the RDP the three bits are not interchangeable. CVG_X_ALPHA only scales
 * coverage by the pipeline alpha; ALPHA_CVG_SEL is what substitutes that scaled
 * coverage for the blender's A_IN input, which is what turns the pair into the
 * "one-bit stencil" a TEX_EDGE mode relies on; FORCE_BL keeps the blender
 * running on every pixel so the pipeline alpha survives as a real blend factor.
 * A cutout is CVG_X_ALPHA and ALPHA_CVG_SEL together, without FORCE_BL --
 * exactly the signature of G_RM_*_TEX_EDGE / TEX_INTER / TEX_TERR.
 *
 * DKR forked a render mode around precisely this distinction:
 * G_RM_AA_ZB_XLU_LINE_MOD (game/include/f3ddkr.h) is documented in the game
 * source as "modified version of RM_AA_ZB_XLU_LINE, with ALPHA_CVG_SEL
 * disabled" and carries CVG_X_ALPHA | FORCE_BL | GBL(CLR_IN, A_IN, CLR_MEM,
 * 1MA) -- an ordinary alpha blend. Testing CVG_X_ALPHA alone collapses that
 * authored blend into a 0.19 alpha-test cutout.
 *
 * MDKR_TEXEDGE=legacy restores the CVG_X_ALPHA-only test for A/B comparison.
 */
static bool dkr_other_mode_l_is_cutout(uint32_t other_mode_l) {
    if (dkr_texedge_legacy_enabled()) {
        return (other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    }
    return (other_mode_l & (CVG_X_ALPHA | ALPHA_CVG_SEL)) ==
               (CVG_X_ALPHA | ALPHA_CVG_SEL) &&
           (other_mode_l & FORCE_BL) == 0;
}

static bool dkr_rdp_gradient_legacy_enabled(void) {
    if (dkr_rdp_gradient_legacy < 0) {
        const char *value = getenv("MDKR_RDP_GRADIENTS");
        dkr_rdp_gradient_legacy =
            value != NULL && strcmp(value, "legacy") == 0;
    }
    return dkr_rdp_gradient_legacy != 0;
}

static GfxRl1Arm dkr_rl1_arm_get(void) {
    if (dkr_rl1_arm < 0) {
        const char *value = getenv("MDKR_RL1_ARM");
        dkr_rl1_arm = GFX_RL1_BAKED;
        if (value != NULL && strcmp(value, "baked-sun") == 0) {
            dkr_rl1_arm = GFX_RL1_BAKED_SUN;
        } else if (value != NULL && strcmp(value, "supersede") == 0) {
            dkr_rl1_arm = GFX_RL1_SUPERSEDE;
        }
    }
    return (GfxRl1Arm) dkr_rl1_arm;
}

const char *gfx_dkr_rl1_active_arm_name(void) {
    return gfx_rl1_arm_name(dkr_rl1_arm_get());
}

static bool dkr_rl5_lighting_enabled(void) {
    if (dkr_rl5_enabled < 0) {
        const char *value = getenv("MDKR_RL5_LIGHT");
        dkr_rl5_enabled =
            value == NULL ||
            (strcmp(value, "0") != 0 &&
             strcmp(value, "off") != 0);
    }
    return dkr_rl5_enabled != 0;
}

const char *gfx_dkr_rl5_active_arm_name(void) {
    return dkr_rl5_lighting_enabled() ? "smooth-sun" : "baked";
}

/* Set while emitting a TEXTURE RECTANGLE, whose s/t are absolute RDP texel
 * coordinates. The non-perspective (G_TP_NONE) *0.5 texcoord halving models the
 * RSP's fixed texcoord scale when it computes perspective-off GEOMETRY texcoords
 * — it must NOT be applied to TEXRECT coords, which the RDP consumes directly.
 * DKR draws all text and the title logo as G_TP_NONE texrects, so without this
 * guard every glyph/logo strip samples at half scale (2x zoom → mangled text,
 * squished logo). */

static void dkr_apply_tile_uv(float *u, float *v, const struct DkrTile *t) {
    if (t->shifts) { if (t->shifts <= 10) *u /= (float)(1 << t->shifts);
                     else *u *= (float)(1 << (16 - t->shifts)); }
    if (t->shiftt) { if (t->shiftt <= 10) *v /= (float)(1 << t->shiftt);
                     else *v *= (float)(1 << (16 - t->shiftt)); }
    *u -= t->uls / 4.0f;
    *v -= t->ult / 4.0f;
    if (!dkr_in_texrect && !(rdp.other_mode_h & G_TP_PERSP)) { *u *= 0.5f; *v *= 0.5f; }
    if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) { *u += 0.5f; *v += 0.5f; }
}

/*
 * RSP clip-ratio emulation.
 *
 * On the N64 the viewport is ONLY the NDC->screen affine map. Triangle clipping
 * happens against the clip volume scaled by the ratio in gSPClipRatio, and the
 * RDP scissor is what actually bounds the rasterized image. DKR's RSP init sets
 * gsSPClipRatio(FRUSTRATIO_2) (rcp_dkr.c dRspInit), so geometry survives out to
 * twice the viewport extent and the scissor decides where the picture stops.
 *
 * A host GPU has no such knob: it clips at NDC +-1, which IS the viewport
 * rectangle. So wherever an authored scissor reaches past its authored
 * viewport, world geometry stops at the viewport edge and whatever the frame
 * clear left behind shows through instead.
 *
 * PAL gameplay is exactly that case. camera.c's viewport_scissor_set() shifts
 * the one-player viewport centre four logical pixels left on PAL
 * ("if (osTvType == OS_TV_TYPE_PAL) posX -= 4;") while leaving the scissor at
 * the full 320x264 surface, so logical columns 316..319 sit outside the
 * viewport. Hardware fills them from the clip-ratio-2 overdraw -- an ares PAL
 * capture of the Ancient Lake start line shows scene content in every visible
 * framebuffer column -- but here they rendered as the frame-clear colour, a
 * flat band that changed hue every frame (24 host pixels wide at 1920).
 *
 * The fix widens the rectangle handed to the backend until it covers the
 * scissor, capped at the ratio-2 box the RSP would really have clipped to, and
 * folds the difference back into clip space so the world-to-window mapping is
 * bit-for-bit the same affine transform:
 *
 *   window_x = Vx + (ndc + 1)/2 * Vw                          (authored)
 *            = Ex + (ndc * Vw/Ew + bias_x + 1)/2 * Ew         (expanded)
 *   bias_x   = (2*(Vx - Ex) + Vw - Ew) / Ew
 *
 * Where the viewport already covers the scissor -- every NTSC/MPAL path, and
 * every screen-space rectangle, which sets viewport == scissor == drawable --
 * this is the identity and the emitter skips it entirely, so those frames stay
 * byte-identical.
 */
static void dkr_update_clip_expansion(void) {
    const struct XYWidthHeight vp = rdp.viewport;
    const struct XYWidthHeight sc = rdp.scissor;

    rdp.clip_viewport = vp;
    rdp.clip_scale_x = 1.0f;
    rdp.clip_scale_y = 1.0f;
    rdp.clip_bias_x = 0.0f;
    rdp.clip_bias_y = 0.0f;
    rdp.clip_expanded = false;
    if (vp.width <= 0 || vp.height <= 0) return;

    /*
     * The overhang has to be worth at least one AUTHORED pixel before it counts.
     * The game states both rectangles in whole 320x240/264 units, so on hardware
     * they either overlap exactly or differ by whole framebuffer columns; a
     * mapped gap thinner than one authored pixel is this port's own rounding
     * (lroundf on two separately scaled rectangles), not geometry the RSP would
     * have drawn. Without this floor the framed menu world views -- whose
     * mapped viewport lands half an authored pixel inside its scissor on NTSC
     * as well -- would be nudged, changing US output for nothing.
     */
    float unit_x = (rdp.logical_viewport_valid && rdp.logical_viewport.width > 0.0f)
        ? (float)vp.width / rdp.logical_viewport.width : 1.0f;
    float unit_y = (rdp.logical_viewport_valid && rdp.logical_viewport.height > 0.0f)
        ? (float)vp.height / rdp.logical_viewport.height : 1.0f;
    if (!(unit_x >= 1.0f)) unit_x = 1.0f;
    if (!(unit_y >= 1.0f)) unit_y = 1.0f;

    int32_t x0 = vp.x, x1 = vp.x + vp.width;
    int32_t y0 = vp.y, y1 = vp.y + vp.height;
    if (sc.width > 0) {
        if ((float)(vp.x - sc.x) >= unit_x) x0 = sc.x;
        if ((float)((sc.x + sc.width) - (vp.x + vp.width)) >= unit_x)
            x1 = sc.x + sc.width;
    }
    if (sc.height > 0) {
        if ((float)(vp.y - sc.y) >= unit_y) y0 = sc.y;
        if ((float)((sc.y + sc.height) - (vp.y + vp.height)) >= unit_y)
            y1 = sc.y + sc.height;
    }
    /* FRUSTRATIO_2 stops here: the RSP keeps geometry out to twice the viewport
     * extent about the viewport centre and discards the rest. A scissor wider
     * than that leaves genuinely unpainted columns on hardware too. */
    int32_t half_w = vp.width / 2, half_h = vp.height / 2;
    if (x0 < vp.x - half_w) x0 = vp.x - half_w;
    if (x1 > vp.x + vp.width + half_w) x1 = vp.x + vp.width + half_w;
    if (y0 < vp.y - half_h) y0 = vp.y - half_h;
    if (y1 > vp.y + vp.height + half_h) y1 = vp.y + vp.height + half_h;

    if (x0 == vp.x && x1 == vp.x + vp.width &&
        y0 == vp.y && y1 == vp.y + vp.height) return;

    float ew = (float)(x1 - x0);
    float eh = (float)(y1 - y0);
    if (!(ew > 0.0f) || !(eh > 0.0f)) return;

    rdp.clip_viewport.x = x0;
    rdp.clip_viewport.y = y0;
    rdp.clip_viewport.width = x1 - x0;
    rdp.clip_viewport.height = y1 - y0;
    rdp.clip_scale_x = (float)vp.width / ew;
    rdp.clip_scale_y = (float)vp.height / eh;
    rdp.clip_bias_x = (2.0f * (float)(vp.x - x0) + (float)vp.width - ew) / ew;
    rdp.clip_bias_y = (2.0f * (float)(vp.y - y0) + (float)vp.height - eh) / eh;
    rdp.clip_expanded = true;
}

/* Set up every GPU state required for the current material, and cache the
 * feature/tex info the emitter needs. Flushes on any state change. */
/*
 * Prepare shader, texture, depth, viewport and blend state for the primitives
 * that follow.
 *
 * Returns false when a texture the generated shader SAMPLES could not be bound.
 * That is not cosmetic: the failed unit keeps whatever the previous draw left
 * there, and cur.tex_w/tex_h fall back to 1x1, so the primitive would rasterize
 * with an unrelated image stretched across it. The caller must skip the draw.
 * Every state change above the texture stage is already committed when this
 * happens, which is harmless -- the next successful setup re-derives all of it.
 */
static bool dkr_setup_draw_state(bool poly_tex_enabled) {
    /*
     * A draw-space matrix is decoded before its first primitive. That gives us
     * one exact boundary at which all queued world triangles can be flushed,
     * the scene resolved, and subsequent HUD geometry emitted at the physical
     * output resolution.
     */
    dkr_prepare_draw_target();

    uint64_t cc_id = rdp.combine_mode;

    /* Blend / render-mode classification (Emill fast3d fallback path). */
    bool blend_alpha = (rdp.other_mode_l & (3U << 20)) == ((uint32_t)G_BL_CLR_MEM << 20) &&
                       (rdp.other_mode_l & (3U << 16)) == ((uint32_t)G_BL_1MA << 16);
    bool texture_edge = dkr_other_mode_l_is_cutout(rdp.other_mode_l);
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;
    bool is_2cyc = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    enum GfxBlendMode blend_mode = (blend_alpha || texture_edge)
                                       ? GFX_BLEND_ALPHA : GFX_BLEND_DISABLED;
    bool use_alpha = (blend_mode != GFX_BLEND_DISABLED);
    int shadow_view = -1;
    bool shadow_receiver = false;

    if (g_pcRemasterFX && g_pcSunShadow &&
        rsp.draw_space == G_MTX_DKR_SPACE_WORLD &&
        !rsp.billboard && !dkr_in_texrect) {
        const float viewport[4] = {
            rdp.logical_viewport.x,
            rdp.logical_viewport.y,
            rdp.logical_viewport.width,
            rdp.logical_viewport.height,
        };
        shadow_view = gfx_shadow_previous_view_index(viewport);
        shadow_receiver =
            shadow_view >= 0 &&
            shadow_view < GFX_SHADOW_MAX_VIEWS &&
            rsp.active_slot >= 0 && rsp.active_slot < 3 &&
            rsp.shadow_matrix_valid[rsp.active_slot] &&
            (g_pc_shadow_view_ready_mask &
             (1u << (unsigned)shadow_view)) != 0 &&
            (rsp.geometry_mode & G_ZBUFFER) != 0 &&
            (rdp.other_mode_l & Z_CMP) == Z_CMP &&
            (rdp.other_mode_l & ZMODE_DEC) != ZMODE_DEC &&
            (!blend_alpha || texture_edge);
    }
    if (shadow_view != dkr_shadow_receiver_view) {
        gfx_flush();
        dkr_shadow_receiver_view = shadow_view;
        if (gfx_rapi != NULL && gfx_rapi->set_shadow_view != NULL) {
            gfx_rapi->set_shadow_view(shadow_view);
        }
    }

    uint32_t cc_options = 0;
    if (use_alpha)     cc_options |= SHADER_OPT_ALPHA;
    if (use_fog)       cc_options |= SHADER_OPT_FOG;
    if (texture_edge)  cc_options |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise)     cc_options |= SHADER_OPT_NOISE;
    if (is_2cyc)       cc_options |= SHADER_OPT_2CYC;
    if (shadow_receiver) {
        cc_options |= SHADER_OPT_WORLD_POS | SHADER_OPT_SUN_SHADOW;
    }
    if (g_pcRemasterFX && g_pcPerPixelLight &&
        dkr_rl5_lighting_enabled() &&
        g_pcLevelLightingValid &&
        rsp.remaster_light_class != 0 &&
        rsp.smooth_normals != NULL &&
        rsp.draw_space == G_MTX_DKR_SPACE_WORLD &&
        !rsp.billboard && !dkr_in_texrect) {
        cc_options |= SHADER_OPT_DFDX_LIGHT;
    }
    cc_options |= gfx_rdp_interpolation_options(
        use_fog, dkr_rdp_gradient_legacy_enabled());

    struct ColorCombiner *comb = dkr_lookup_or_create_combiner(cc_id, cc_options);
    gfx_cc_get_features(comb->shader_id0, comb->shader_id1, &cur.feat);
    cur.comb = comb;
    cur.num_inputs = cur.feat.num_inputs;
    cur.use_fog = cur.feat.opt_fog;
    cur.tile_base = rsp.tile_base;
    /* The generated shader (derived from the combiner) is the source of truth
     * for whether a texture is sampled; poly_tex_enabled is DKR's advisory
     * TRIN texture flag, retained for reference. */
    (void)poly_tex_enabled;
    cur.use_texture = cur.feat.used_textures[0] || cur.feat.used_textures[1];

    /* A failed creation must not be cached: the combiner pool is regenerable and
     * a NULL program here would make this cc_id permanently undrawable. */
    if (comb->prg == NULL) {
        comb->prg = dkr_lookup_or_create_shader(
            comb->shader_id0, comb->shader_id1);
    }

    /* A backend that could not build the program leaves the previously bound one
     * in place; the vtable takes no NULL program. */
    if (comb->prg != NULL && comb->prg != rendering_state.shader_program) {
        gfx_flush();
        /* The attribute set differs per program; without the unload the arrays a
         * wider program enabled stay enabled and fetch past a narrower one's VBO
         * stride. gfx_flush above guarantees no batch spans the switch. */
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(comb->prg);
        rendering_state.shader_program = comb->prg;
    }

    /* Textures — bind whichever units the shader samples. */
    bool bind_ok[2] = { true, true };
    for (int i = 0; i < 2; i++) {
        cur.tex_w[i] = 1; cur.tex_h[i] = 1;
        if (cur.feat.used_textures[i]) {
            uint8_t td = (uint8_t)(cur.tile_base + i);
            if (td >= 8) td = 0;
            uint32_t w = 1, h = 1;
            bind_ok[i] = dkr_bind_tile(i, td, texture_edge, &w, &h);
            if (bind_ok[i]) {
                cur.tex_w[i] = w; cur.tex_h[i] = h;
            } else {
                /* One line, once per process: a bind failure is a texture the
                 * backend could not produce (no loaded TMEM image, or a
                 * refused texture object), which is a state worth knowing about
                 * without turning a hot path into a log. The DTRACE field above
                 * keeps the per-draw detail for a traced frame. */
                static bool traced_bind_failure = false;
                if (!traced_bind_failure) {
                    traced_bind_failure = true;
                    fprintf(stderr,
                            "[gfx] texture bind failed (unit=%d tile=%u); "
                            "affected draws are skipped rather than drawn with "
                            "the previously bound image\n", i, (unsigned)td);
                }
            }
        }
    }
    if (dkr_trace_this_frame) {
        DTRACE("  setup: cc_id=%016llx opt=%03x shader_id0=%016llx numin=%d useTex=%d "
               "usedTex[%d,%d] bindOK[%d,%d] texWH0=%ux%u tileBase=%d",
               (unsigned long long)cc_id, cc_options,
               (unsigned long long)comb->shader_id0, cur.num_inputs, cur.use_texture,
               cur.feat.used_textures[0], cur.feat.used_textures[1],
               bind_ok[0], bind_ok[1], cur.tex_w[0], cur.tex_h[0], cur.tile_base);
    }

    /* Depth mode. */
    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) != 0 && (rdp.other_mode_l & Z_CMP) != 0;
    bool depth_update = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    bool depth_compare = (rdp.other_mode_l & Z_CMP) == Z_CMP;
    bool depth_source_prim = (rdp.other_mode_l & G_ZS_PRIM) == G_ZS_PRIM;
    uint16_t zmode = rdp.other_mode_l & ZMODE_DEC;
    uint8_t depth_mode = (depth_test ? 1 : 0) | (depth_update ? 2 : 0) |
                         (depth_compare ? 4 : 0) | (depth_source_prim ? 8 : 0) |
                         (uint8_t)(zmode >> 6);
    if (depth_mode != rendering_state.depth_mode) {
        gfx_flush();
        gfx_rapi->set_depth_mode(depth_test, depth_update, depth_compare, depth_source_prim, zmode);
        rendering_state.depth_mode = depth_mode;
    }

    /* Viewport / scissor. */
    if (rdp.viewport_or_scissor_changed) {
        dkr_update_clip_expansion();
        /* Buffered triangles carry the clip factors that were live when they
         * were emitted, so a change in either the effective rectangle or those
         * factors has to flush first. */
        bool viewport_changed =
            memcmp(&rdp.clip_viewport, &rendering_state.viewport,
                   sizeof(rdp.clip_viewport)) != 0;
        bool clip_changed =
            rdp.clip_scale_x != rendering_state.clip_scale_x ||
            rdp.clip_scale_y != rendering_state.clip_scale_y ||
            rdp.clip_bias_x != rendering_state.clip_bias_x ||
            rdp.clip_bias_y != rendering_state.clip_bias_y;
        if (viewport_changed || clip_changed) {
            gfx_flush();
            if (viewport_changed) {
                gfx_rapi->set_viewport(rdp.clip_viewport.x, rdp.clip_viewport.y,
                                       rdp.clip_viewport.width,
                                       rdp.clip_viewport.height);
                rendering_state.viewport = rdp.clip_viewport;
            }
            rendering_state.clip_scale_x = rdp.clip_scale_x;
            rendering_state.clip_scale_y = rdp.clip_scale_y;
            rendering_state.clip_bias_x = rdp.clip_bias_x;
            rendering_state.clip_bias_y = rdp.clip_bias_y;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y,
                                  rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }

    if (blend_mode != rendering_state.blend_mode) {
        gfx_flush();
        gfx_rapi->set_blend_mode(blend_mode);
        rendering_state.blend_mode = blend_mode;
    }
    return bind_ok[0] && bind_ok[1];
}

/* ------------------------------------------------------------------------- */
/* Triangle emission                                                         */
/* ------------------------------------------------------------------------- */

/* Compute the final normalized VBO texcoord for a corner (S10.5 in u/v). The
 * gSPTexture s/t scale (0.16 fixed, default 0xFFFF≈1.0) multiplies the raw
 * coords first, mirroring the N64 `tc * scale >> 16` step. */
static void dkr_vbo_texcoord(const struct LoadedVertex *vtx, int ti, float *ou, float *ov) {
    /* F3DDKR texture-coordinate scale: unlike stock F3DEX, DKR's gSPTexture always
     * emits an s/t scale of 0x0000 (verified: every G_TEXTURE in the stream has
     * scaleS=scaleT=0). DKR's Triangle and TEXRECT texture coordinates are already
     * ABSOLUTE S10.5 texel values, so the microcode does not use the gSPTexture
     * scale as a coordinate multiplier — an F3DEX-style `tc * scale >> 16` would
     * multiply by zero and collapse every texcoord to (0,0), sampling texel 0 of
     * the tile for the whole primitive (this is what rendered all menu text as
     * solid boxes and all textured geometry — terrain, sprites — as flat colour).
     * Treat a zero scale as unity (0x10000 == 1.0). */
    float ss = rsp.tex_scale_s ? (rsp.tex_scale_s / 65536.0f) : 1.0f;
    float st = rsp.tex_scale_t ? (rsp.tex_scale_t / 65536.0f) : 1.0f;
    float u = vtx->u * ss / 32.0f;
    float v = vtx->v * st / 32.0f;
    uint8_t td = (uint8_t)(cur.tile_base + ti);
    if (td >= 8) td = 0;
    dkr_apply_tile_uv(&u, &v, &rdp.tile[td]);
    *ou = u / (float)(cur.tex_w[ti] ? cur.tex_w[ti] : 1);
    *ov = v / (float)(cur.tex_h[ti] ? cur.tex_h[ti] : 1);
}

/* Append one already-transformed triangle to the VBO in the exact attribute
 * order the generated backend shader expects (Emill fast3d layout):
 *   aVtxPos(4) [ aWorldPos(3) ] [ aSmoothNormal(3) aLightDir(3) ]
 *   [ aTexCoord0(2) ] [ aTexCoord1(2) ] [ aFog(4) ] aInputN(3|4)...
 *
 * Keep this order in lockstep with every backend's generated attribute
 * declarations. World position used to be declared but never packed, which
 * shifted every later attribute and turned a shadow-enabled draw into
 * malformed full-screen geometry.
 */
static int dkr_dbg_emitted, dkr_dbg_onscreen;   /* per-frame draw telemetry */

static void dkr_emit_tri(const struct LoadedVertex *v0,
                         const struct LoadedVertex *v1,
                         const struct LoadedVertex *v2) {
    const struct LoadedVertex *vs[3] = { v0, v1, v2 };
    bool z01 = gfx_rapi->z_is_from_0_to_1();
    bool software_far_clamp = false;
#ifdef MDKR_WEBGPU_BACKEND
    /*
     * DepthClipControl is optional. Without it WebGPU clips a triangle that
     * crosses z=w, while the original renderer and the GL/Metal paths clamp its
     * depth and preserve the far terrain. Clamping homogeneous z before the
     * backend's [-w,+w] -> [0,w] conversion is exactly the missing fixed
     * function behavior and needs no new vertices.
     */
    software_far_clamp =
        mdkr_render_backend() == MDKR_BACKEND_WEBGPU &&
        !gfx_webgpu_unclipped_depth_supported();
#endif
    if (buf_vbo_num_tris + 1 > DKR_MAX_BUFFERED) gfx_flush();

    /*
     * Observe the state after the display-list interpreter has decoded it, at
     * the exact primitive boundary consumed by dkr_setup_draw_state(). Counting
     * source-level shadow flags would miss regressions in render-mode packing,
     * command decoding or state inheritance.
     */
    if ((rsp.geometry_mode & G_ZBUFFER) != 0 &&
        (rdp.other_mode_l & Z_CMP) == Z_CMP) {
        gfx_dkr_depth_compared_triangles++;
        if ((rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC) {
            gfx_dkr_decal_triangles++;
        }
    }
    if (cur.feat.opt_dfdx_light) {
        if (rsp.remaster_light_class == 1) {
            gfx_dkr_remaster_racer_triangles++;
        } else if (rsp.remaster_light_class == 2) {
            gfx_dkr_remaster_character_triangles++;
        }
    }

    dkr_dbg_emitted++;
    { /* count triangles that place any corner inside the NDC box */
        int on = 0;
        for (int i = 0; i < 3; i++) {
            float w = vs[i]->w; if (w <= 0) continue;
            float nx = vs[i]->x / w, ny = vs[i]->y / w;
            if (nx > -1.05f && nx < 1.05f && ny > -1.05f && ny < 1.05f) { on = 1; break; }
        }
        dkr_dbg_onscreen += on;
    }

    bool use_alpha = cur.feat.opt_alpha;
    for (int i = 0; i < 3; i++) {
        const struct LoadedVertex *vt = vs[i];
        float z = vt->z, w = vt->w;
        if (software_far_clamp && z > w) {
            z = w;
        }
        if (z01) z = (z + w) / 2.0f;
        /*
         * The N64 mirrors Adventure Two with a negative viewport X scale.
         * GL and WebGPU require a non-negative viewport width, so
         * dkr_calc_viewport() retains the sign here and publishes the absolute
         * host rectangle. Negating clip X is algebraically identical:
         *   center + ndcX * -scale == center + (-ndcX) * scale.
         * This must happen at emission rather than in the world matrix because
         * the same display list later installs a positive viewport for its
         * safe-4:3 HUD.
         */
        float clip_x = rdp.viewport_flip_x ? -vt->x : vt->x;
        float clip_y = vt->y;
        if (rdp.clip_expanded) {
            /* The backend viewport was widened to the scissor to stand in for
             * the RSP's clip ratio (dkr_update_clip_expansion). Re-express the
             * same NDC in the wider rectangle so the picture does not move;
             * skipped outright when the expansion is the identity, which keeps
             * every unexpanded frame bit-identical. */
            clip_x = clip_x * rdp.clip_scale_x + vt->w * rdp.clip_bias_x;
            clip_y = clip_y * rdp.clip_scale_y + vt->w * rdp.clip_bias_y;
        }
        buf_vbo[buf_vbo_len++] = clip_x;
        buf_vbo[buf_vbo_len++] = clip_y;
        buf_vbo[buf_vbo_len++] = z;
        buf_vbo[buf_vbo_len++] = w;

        if (cur.feat.opt_world_pos) {
            /* R13 instrument: a receiver draw consuming a vertex whose world
             * position was never derived (loaded under a billboard or an
             * invalid slot, drawn in a world state) samples the shadow map at
             * the origin. Counted so the condition is measurable rather than
             * assumed; the counter must stay zero on the production routes. */
            if (!vt->world_valid) {
                gfx_dkr_shadow_receiver_invalid_world++;
            }
            buf_vbo[buf_vbo_len++] = vt->world_x;
            buf_vbo[buf_vbo_len++] = vt->world_y;
            buf_vbo[buf_vbo_len++] = vt->world_z;
        }

        if (cur.feat.opt_dfdx_light) {
            buf_vbo[buf_vbo_len++] = vt->normal_x;
            buf_vbo[buf_vbo_len++] = vt->normal_y;
            buf_vbo[buf_vbo_len++] = vt->normal_z;
            buf_vbo[buf_vbo_len++] = vt->light_x;
            buf_vbo[buf_vbo_len++] = vt->light_y;
            buf_vbo[buf_vbo_len++] = vt->light_z;
        }

        for (int ti = 0; ti < 2; ti++) {
            if (!cur.feat.used_textures[ti]) continue;
            float uu, vv; dkr_vbo_texcoord(vt, ti, &uu, &vv);
            buf_vbo[buf_vbo_len++] = uu;
            buf_vbo[buf_vbo_len++] = vv;
        }

        if (cur.use_fog) {
            buf_vbo[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            buf_vbo[buf_vbo_len++] = vt->fog / 255.0f;
        }

        for (int j = 0; j < cur.num_inputs; j++) {
            for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
                struct RGBA c; struct RGBA tmp;
                switch (cur.comb->shader_input_mapping[k][j]) {
                    case G_CCMUX_PRIMITIVE:      c = rdp.prim_color; break;
                    case G_CCMUX_SHADE:          c = vt->color; break;
                    case G_CCMUX_ENVIRONMENT:    c = rdp.env_color; break;
                    case G_CCMUX_PRIMITIVE_ALPHA: tmp.r = tmp.g = tmp.b = tmp.a = rdp.prim_color.a; c = tmp; break;
                    case G_CCMUX_SHADE_ALPHA:    tmp.r = tmp.g = tmp.b = tmp.a = vt->color.a; c = tmp; break;
                    case G_CCMUX_ENV_ALPHA:      tmp.r = tmp.g = tmp.b = tmp.a = rdp.env_color.a; c = tmp; break;
                    case G_CCMUX_PRIM_LOD_FRAC:  tmp.r = tmp.g = tmp.b = tmp.a = rdp.prim_lod_fraction; c = tmp; break;
                    case G_CCMUX_LOD_FRACTION:   tmp.r = tmp.g = tmp.b = tmp.a = 255; c = tmp; break;
                    default:                     memset(&tmp, 0, sizeof(tmp)); c = tmp; break;
                }
                if (k == 0) {
                    buf_vbo[buf_vbo_len++] = c.r / 255.0f;
                    buf_vbo[buf_vbo_len++] = c.g / 255.0f;
                    buf_vbo[buf_vbo_len++] = c.b / 255.0f;
                } else {
                    buf_vbo[buf_vbo_len++] = c.a / 255.0f;
                }
            }
        }
    }
    buf_vbo_num_tris++;
}

/* ------------------------------------------------------------------------- */
/* Near-plane clipping                                                       */
/* ------------------------------------------------------------------------- */
/*
 * Every triangle is clipped against the near plane in HOMOGENEOUS CLIP SPACE
 * before it reaches the VBO. Previously the HLE emitted triangles unclipped and
 * leaned on GL_DEPTH_CLAMP, which does not clip — it only stops depth testing
 * from discarding out-of-range fragments. A vertex behind the eye (w <= 0)
 * therefore still went through the perspective divide, and dividing by a
 * negative w mirrors the vertex through the origin, flinging that corner to the
 * far side of the screen: the triangle renders as a long stretched sliver
 * instead of being cut at the near plane. That is the "stretched bar / spike"
 * artifact class (see docs/OPEN_ITEMS.md).
 *
 * The plane is `z + w >= 0`, i.e. z_ndc >= -1, which is the TRUE near plane and
 * not merely `w > 0`. For a well-formed perspective projection the two are
 * equivalent in the useful direction: writing z = -(f+n)/(f-n)·z_e - 2fn/(f-n)
 * and w = -z_e gives z + w = (-2f/(f-n))·(z_e + n), so z + w >= 0 iff
 * z_e <= -n, and every such point has w = -z_e >= n > 0. So clipping on this one
 * plane both cuts at the correct depth and guarantees a positive w downstream.
 *
 * Clipping happens in clip space, before the divide. Position, UV and shade
 * attributes are evaluated at that homogeneous edge intersection. Fog is the
 * deliberate exception: its stored byte has already been derived from
 * post-divide z/w, so a generated vertex must recompute it from its new
 * position rather than blend two endpoint bytes. During rasterization texture
 * coordinates remain perspective-correct, while RDP shade and fog coefficients
 * are explicitly screen-linear.
 *
 * Sutherland-Hodgman against a single plane turns a triangle into at most a
 * quad, which is then fan-triangulated. Deliberately NOT a cull: dropping
 * straddling triangles would punch holes in geometry that legitimately crosses
 * the near plane.
 */
#define DKR_CLIP_MAX_VERTS 4 /* a triangle clipped by one plane yields <= 4 */

/* Per-frame clip telemetry (reported by gfx_run under MDKR_TRACE>=2). */
static int dkr_dbg_clipped;      /* triangles the near plane actually cut     */
static int dkr_dbg_clip_dropped; /* triangles entirely behind the near plane  */
static int dkr_dbg_clip_degen;   /* clipped but left with a non-positive w    */

static uint8_t dkr_lerp_u8(uint8_t a, uint8_t b, float t) {
    float v = (float)a + ((float)b - (float)a) * t;
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

/* out = a + (b - a) * t, for every attribute the VBO carries. */
static void dkr_lerp_vertex(struct LoadedVertex *out,
                            const struct LoadedVertex *a,
                            const struct LoadedVertex *b, float t) {
    out->x = a->x + (b->x - a->x) * t;
    out->y = a->y + (b->y - a->y) * t;
    out->z = a->z + (b->z - a->z) * t;
    out->w = a->w + (b->w - a->w) * t;
    out->model_x = a->model_x + (b->model_x - a->model_x) * t;
    out->model_y = a->model_y + (b->model_y - a->model_y) * t;
    out->model_z = a->model_z + (b->model_z - a->model_z) * t;
    out->world_x = a->world_x + (b->world_x - a->world_x) * t;
    out->world_y = a->world_y + (b->world_y - a->world_y) * t;
    out->world_z = a->world_z + (b->world_z - a->world_z) * t;
    out->world_valid = a->world_valid && b->world_valid;
    out->normal_x = a->normal_x + (b->normal_x - a->normal_x) * t;
    out->normal_y = a->normal_y + (b->normal_y - a->normal_y) * t;
    out->normal_z = a->normal_z + (b->normal_z - a->normal_z) * t;
    out->light_x = a->light_x;
    out->light_y = a->light_y;
    out->light_z = a->light_z;
    out->u = a->u + (b->u - a->u) * t;
    out->v = a->v + (b->v - a->v) * t;
    out->color.r = dkr_lerp_u8(a->color.r, b->color.r, t);
    out->color.g = dkr_lerp_u8(a->color.g, b->color.g, t);
    out->color.b = dkr_lerp_u8(a->color.b, b->color.b, t);
    out->color.a = dkr_lerp_u8(a->color.a, b->color.a, t);
    if ((rsp.geometry_mode & G_FOG) != 0 &&
        !dkr_rdp_gradient_legacy_enabled()) {
        out->fog = gfx_rdp_fog_from_clip(
            out->z, out->w, rsp.fog_mul, rsp.fog_offset);
    } else {
        out->fog = dkr_lerp_u8(a->fog, b->fog, t);
    }
}

/* Clip a convex polygon against z + w >= 0. Returns the output vertex count
 * (0, or 3..DKR_CLIP_MAX_VERTS); `out` must hold DKR_CLIP_MAX_VERTS entries. */
/*
 * MDKR_NEARCLIP selects the clip plane, for A/B measurement against the pre-fix
 * behaviour (same convention as MDKR_VI_PACE=off and MDKR_AUDIO_REVERB=0):
 *   zw  (default) clip at z + w >= 0 — the true near plane.
 *   w             clip at w > 0 only. Removes the behind-the-eye mirroring but
 *                 NOT geometry between the eye and the near plane; kept because
 *                 it isolates "was it the mirroring or the plane depth?".
 *   off           emit unclipped, reproducing the pre-fix artifact.
 */
static int dkr_clip_mode = -1;
static int dkr_clip_mode_get(void) {
    if (dkr_clip_mode < 0) {
        const char *s = getenv("MDKR_NEARCLIP");
        dkr_clip_mode = 0; /* 0 = z+w */
        if (s && s[0] == 'w') dkr_clip_mode = 1;
        else if (s && s[0] == 'o') dkr_clip_mode = 2;
    }
    return dkr_clip_mode;
}
static float dkr_clip_dist(const struct LoadedVertex *v) {
    return (dkr_clip_mode_get() == 1) ? (v->w - 1e-5f) : (v->z + v->w);
}

static int dkr_clip_near(const struct LoadedVertex *in, int n,
                         struct LoadedVertex *out) {
    int count = 0;
    int i;

    if (dkr_clip_mode_get() == 2) { /* off — reproduce the pre-clip behaviour */
        for (i = 0; i < n; i++) out[i] = in[i];
        return n;
    }
    for (i = 0; i < n; i++) {
        const struct LoadedVertex *cur = &in[i];
        const struct LoadedVertex *nxt = &in[(i + 1) % n];
        float dc = dkr_clip_dist(cur);
        float dn = dkr_clip_dist(nxt);
        int cur_in = (dc >= 0.0f);
        int nxt_in = (dn >= 0.0f);

        if (cur_in && count < DKR_CLIP_MAX_VERTS) {
            out[count++] = *cur;
        }
        if (cur_in != nxt_in && count < DKR_CLIP_MAX_VERTS) {
            float denom = dc - dn;
            if (denom != 0.0f) {
                float t = dc / denom;
                if (t < 0.0f) t = 0.0f;
                else if (t > 1.0f) t = 1.0f;
                dkr_lerp_vertex(&out[count++], cur, nxt, t);
            }
        }
    }
    return count;
}

/* Screen-space signed area in NDC (>0 CCW). Used for backface culling. */
static float dkr_ndc_cross(const struct LoadedVertex *a,
                           const struct LoadedVertex *b,
                           const struct LoadedVertex *c) {
    float aw = a->w != 0 ? a->w : 1e-6f;
    float bw = b->w != 0 ? b->w : 1e-6f;
    float cw = c->w != 0 ? c->w : 1e-6f;
    float ax = a->x / aw, ay = a->y / aw;
    float bx = b->x / bw, by = b->y / bw;
    float cx = c->x / cw, cy = c->y / cw;
    return (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
}

/* ------------------------------------------------------------------------- */
/* SP: gSPVertexDKR (G_VTX)                                                   */
/* ------------------------------------------------------------------------- */

static void dkr_sp_vertex(const Vertex *verts, int n, bool append,
                          const float (*position_overrides)[3],
                          const struct RGBA *color_overrides) {
    int dest = append ? rsp.vtx_append_pos : 0;
    const float (*m)[4] = rsp.mtx[rsp.active_slot];

    /* Never read a vertex batch past the arena (edge/mis-decoded pointer), and
     * never dereference a non-arena pointer that isn't a plausible registered
     * global (a sign-extended wild pointer from an upstream LP64 truncation). */
    size_t room = dkr_arena_room(verts);
    if (room == (size_t)-1 && !dkr_ptr_plausible(verts)) return;
    if ((size_t)n * sizeof(Vertex) > room) n = (int)(room / sizeof(Vertex));
    if (n <= 0) return;

    const Vec3s *normal_stream = rsp.smooth_normals;
    size_t normal_room =
        normal_stream != NULL ? dkr_arena_room(normal_stream) : 0;
    if (normal_stream != NULL &&
        normal_room != (size_t)-1 &&
        (size_t)n * sizeof(Vec3s) > normal_room) {
        normal_stream = NULL;
        gfx_dkr_remaster_missing_normal_batches++;
    }
    if (normal_stream != NULL &&
        normal_room == (size_t)-1 &&
        !dkr_ptr_plausible(normal_stream)) {
        normal_stream = NULL;
        gfx_dkr_remaster_missing_normal_batches++;
    }

    for (int i = 0; i < n; i++) {
        int di = dest + i;
        if (di < 0 || di >= DKR_MAX_VERTICES) continue;
        struct LoadedVertex *d = &rsp.loaded[di];
        float x = verts[i].x, y = verts[i].y, z = verts[i].z;
        if (position_overrides != NULL) {
            x = position_overrides[i][0];
            y = position_overrides[i][1];
            z = position_overrides[i][2];
        }

        /* Row-vector transform through the selected MVP matrix. */
        float cx = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        float cy = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        float cz = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        float cw = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];

        if (rsp.billboard) {
            /* Billboard vertices add to vertex 0's transformed clip position
             * (after MVP, before perspective divide) and inherit its w. The
             * selected matrix here is the billboard/sprite matrix in slot 2.
             * (f3ddkr.h billboarding notes.) */
            const struct LoadedVertex *anchor = &rsp.loaded[0];
            d->x = anchor->x + cx;
            d->y = anchor->y + cy;
            d->z = anchor->z + cz;
            d->w = anchor->w;
        } else {
            d->x = cx; d->y = cy; d->z = cz; d->w = cw;
        }
        d->model_x = x;
        d->model_y = y;
        d->model_z = z;
        d->world_x = d->world_y = d->world_z = 0.0f;
        d->world_valid = false;
        if (!rsp.billboard &&
            rsp.active_slot >= 0 && rsp.active_slot < 3 &&
            rsp.shadow_matrix_valid[rsp.active_slot]) {
            const float (*world)[4] =
                rsp.shadow_matrix[rsp.active_slot].world;
            d->world_x =
                x * world[0][0] + y * world[1][0] +
                z * world[2][0] + world[3][0];
            d->world_y =
                x * world[0][1] + y * world[1][1] +
                z * world[2][1] + world[3][1];
            d->world_z =
                x * world[0][2] + y * world[1][2] +
                z * world[2][2] + world[3][2];
            d->world_valid =
                isfinite(d->world_x) &&
                isfinite(d->world_y) &&
                isfinite(d->world_z);
        }
        d->normal_x = d->normal_y = d->normal_z = 0.0f;
        if (normal_stream != NULL) {
            float nx = normal_stream[i].x;
            float ny = normal_stream[i].y;
            float nz = normal_stream[i].z;
            float length_sq = nx * nx + ny * ny + nz * nz;
            if (isfinite(length_sq) && length_sq > 1.0e-10f) {
                float inverse = 1.0f / sqrtf(length_sq);
                d->normal_x = nx * inverse;
                d->normal_y = ny * inverse;
                d->normal_z = nz * inverse;
            }
        }
        d->light_x = rsp.light_direction[0];
        d->light_y = rsp.light_direction[1];
        d->light_z = rsp.light_direction[2];

        if (color_overrides != NULL) {
            d->color = color_overrides[i];
        } else {
            d->color.r = verts[i].r;
            d->color.g = verts[i].g;
            d->color.b = verts[i].b;
            d->color.a = verts[i].a;
        }
        d->u = 0.0f; d->v = 0.0f;

        /* Per-vertex fog factor from the transformed depth. */
        if (rsp.geometry_mode & G_FOG) {
            d->fog = gfx_rdp_fog_from_clip(
                d->z, d->w, rsp.fog_mul, rsp.fog_offset);
        } else {
            d->fog = 0;
        }
    }

    /*
     * G_VTX_APPEND is not a running cursor. The F3DDKR RSP keeps ONE count —
     * the length of the last flag-0 load (game/include/f3ddkr.h): a flag-0 load
     * writes at the start of the vertex array and stores its count; every
     * flag-1 load writes immediately after THAT, so two consecutive appends
     * land on the same base and the second overwrites the first.
     *
     * sprite_init_frame() depends on exactly this. A sprite frame wider than
     * one RSP batch is emitted in runs of five quads: each run issues its own
     * appended 20-vertex load and then restarts its triangle indices at vertex
     * 1. Advancing the base per append instead put run 2 at slot 21 while its
     * triangles still named slots 1..4, so every sprite with a sixth tile drew
     * that tile's texture over the FIRST tile's quad and never drew it in its
     * own place — the intro shrubs gained a duplicate piece stacked on top and
     * lost their bottom band, which left them hanging above the ground.
     */
    if (!append) rsp.vtx_append_pos = n;   /* flag-0 count reserves [0,n)      */
}

/* ------------------------------------------------------------------------- */
/* SP: gSPPolygon (G_TRIN) — per-triangle UVs + backface flag                */
/* ------------------------------------------------------------------------- */

/*
 * `uv_offset` is the presentation-only UV-scroll displacement for this batch at
 * the current replay alpha, in the same S10.5 units as the authored corner
 * coordinates, and `uv_offset_mask` names the triangles it applies to (bit i,
 * from the census — drivers exclude TRI_FLAG_80 faces and regenerate others, so
 * membership is recorded rather than re-derived here). NULL on the authoritative
 * walk and at alpha zero, where the authored bytes must reach the backend
 * untouched.
 */
static void dkr_sp_polygon(const Triangle *tris, int num_tris, bool tex_enabled,
                           const float *uv_offset, uint16_t uv_offset_mask_u,
                           uint16_t uv_offset_mask_v) {
    /* A texture the shader samples could not be bound: the unit still holds the
     * previous draw's image, so emitting these triangles would paint an
     * unrelated texture rather than lose one. Drop the batch instead. */
    if (!dkr_setup_draw_state(tex_enabled)) return;
    int emitted = 0, culled = 0, oob = 0;

    /* Never read a triangle batch past the arena (edge/mis-decoded pointer), and
     * never dereference a non-arena pointer that isn't a plausible registered
     * global (a sign-extended wild pointer from an upstream LP64 truncation). */
    size_t room = dkr_arena_room(tris);
    if (room == (size_t)-1 && !dkr_ptr_plausible(tris)) return;
    if ((size_t)num_tris * sizeof(Triangle) > room)
        num_tris = (int)(room / sizeof(Triangle));
    if (num_tris <= 0) return;
    if (g_pcRemasterFX && g_pcSunShadow &&
        gfx_shadow_projected_range_contains(tris)) {
        const float viewport[4] = {
            rdp.logical_viewport.x,
            rdp.logical_viewport.y,
            rdp.logical_viewport.width,
            rdp.logical_viewport.height,
        };
        int view_index = gfx_shadow_previous_view_index(viewport);
        if (view_index >= 0 &&
            view_index < GFX_SHADOW_MAX_VIEWS &&
            (g_pc_shadow_view_ready_mask &
             (1u << (unsigned)view_index)) != 0) {
            return;
        }
    }

    /* Marked at DL-build time: void curtain, wave surfaces, and
     * RENDER_NO_SHADOW batches never enter the caster feed (batch-keyed,
     * like the projected-decal seam above). */
    int caster_excluded = gfx_shadow_caster_excluded(tris);

    for (int i = 0; i < num_tris; i++) {
        const Triangle *t = &tris[i];
        int i0 = t->vi0, i1 = t->vi1, i2 = t->vi2;
        if (i0 >= DKR_MAX_VERTICES || i1 >= DKR_MAX_VERTICES || i2 >= DKR_MAX_VERTICES) {
            oob++;
            continue;
        }

        struct LoadedVertex a = rsp.loaded[i0];
        struct LoadedVertex b = rsp.loaded[i1];
        struct LoadedVertex c = rsp.loaded[i2];

        /*
         * RL-1 is a capture-only decision seam, not a production lighting path.
         * It runs after display-list decoding so both native backends receive
         * exactly the same altered shade values. Restrict it to world geometry
         * whose combiner consumes RGB SHADE; 2D, billboards, alpha-only shade
         * and every Pure/Restored draw remain byte-for-byte untouched.
         */
        if (g_pcRemasterFX &&
            dkr_rl1_arm_get() != GFX_RL1_BAKED &&
            rsp.draw_space == G_MTX_DKR_SPACE_WORLD &&
            !rsp.billboard &&
            !dkr_in_texrect) {
            bool uses_rgb_shade = false;
            for (int input = 0; input < cur.num_inputs; input++) {
                if (cur.comb->shader_input_mapping[0][input] ==
                    G_CCMUX_SHADE) {
                    uses_rgb_shade = true;
                    break;
                }
            }
            if (uses_rgb_shade) {
                const float xyz[9] = {
                    a.model_x, a.model_y, a.model_z,
                    b.model_x, b.model_y, b.model_z,
                    c.model_x, c.model_y, c.model_z,
                };
                uint8_t rgba[12] = {
                    a.color.r, a.color.g, a.color.b, a.color.a,
                    b.color.r, b.color.g, b.color.b, b.color.a,
                    c.color.r, c.color.g, c.color.b, c.color.a,
                };
                if (gfx_rl1_apply_triangle(
                        dkr_rl1_arm_get(), xyz, rgba)) {
                    a.color = (struct RGBA) {
                        rgba[0], rgba[1], rgba[2], rgba[3]
                    };
                    b.color = (struct RGBA) {
                        rgba[4], rgba[5], rgba[6], rgba[7]
                    };
                    c.color = (struct RGBA) {
                        rgba[8], rgba[9], rgba[10], rgba[11]
                    };
                    gfx_dkr_rl1_triangles++;
                }
            }
        }

        /* Per-triangle-corner UVs (S10.5). */
        a.u = (float)(int16_t)t->uv0.u; a.v = (float)(int16_t)t->uv0.v;
        b.u = (float)(int16_t)t->uv1.u; b.v = (float)(int16_t)t->uv1.v;
        c.u = (float)(int16_t)t->uv2.u; c.v = (float)(int16_t)t->uv2.v;
        if (uv_offset != NULL && i < 16) {
            if ((uv_offset_mask_u & (1u << i)) != 0u) {
                a.u += uv_offset[0];
                b.u += uv_offset[0];
                c.u += uv_offset[0];
            }
            if ((uv_offset_mask_v & (1u << i)) != 0u) {
                a.v += uv_offset[1];
                b.v += uv_offset[1];
                c.v += uv_offset[1];
            }
        }

        /*
         * Observe casters before camera near clipping/backface rejection. The
         * display list has already performed game-level admission, but this
         * preserves every submitted face for the light's view. It is strictly
         * capture-only: no game callback and no second DL traversal occurs.
         */
        if ((gfx_world_fx_trace_enabled() ||
             (g_pcRemasterFX && g_pcSunShadow)) &&
            !caster_excluded &&
            rsp.draw_space == G_MTX_DKR_SPACE_WORLD &&
            !rsp.billboard &&
            rsp.active_slot >= 0 && rsp.active_slot < 3 &&
            rsp.shadow_matrix_valid[rsp.active_slot] &&
            a.world_valid && b.world_valid && c.world_valid &&
            (rsp.geometry_mode & G_ZBUFFER) != 0 &&
            (rdp.other_mode_l & Z_CMP) == Z_CMP &&
            (rdp.other_mode_l & ZMODE_DEC) != ZMODE_DEC) {
            bool texture_edge = dkr_other_mode_l_is_cutout(rdp.other_mode_l);
            bool alpha_blend =
                rendering_state.blend_mode == GFX_BLEND_ALPHA;
            float viewport[4] = {
                rdp.logical_viewport.x,
                rdp.logical_viewport.y,
                rdp.logical_viewport.width,
                rdp.logical_viewport.height,
            };
            if (!alpha_blend || texture_edge) {
                float positions[9] = {
                    a.world_x, a.world_y, a.world_z,
                    b.world_x, b.world_y, b.world_z,
                    c.world_x, c.world_y, c.world_z,
                };
                float uv[6] = {0};
                GfxShadowMaterial material = {
                    .texture_id =
                        cur.feat.used_textures[0]
                            ? rendering_state.bound_texture_id[0]
                            : 0,
                    .alpha_mode =
                        texture_edge
                            ? GFX_SHADOW_ALPHA_MASKED
                            : GFX_SHADOW_ALPHA_OPAQUE,
                    .two_sided = (t->flags & BACKFACE_DRAW) != 0,
                };
                int view_index;
                if (cur.feat.used_textures[0]) {
                    dkr_vbo_texcoord(&a, 0, &uv[0], &uv[1]);
                    dkr_vbo_texcoord(&b, 0, &uv[2], &uv[3]);
                    dkr_vbo_texcoord(&c, 0, &uv[4], &uv[5]);
                }
                view_index = gfx_shadow_capture_view(
                    viewport,
                    rsp.shadow_matrix[rsp.active_slot].view_projection);
                (void)gfx_shadow_capture_triangle(
                    view_index,
                    &tris[i],
                    rsp.shadow_matrix[rsp.active_slot].mobility,
                    positions,
                    uv,
                    &material);
            }
        }

        if (dkr_trace_this_frame && i < 2) {
            DTRACE("  tri%d flags=%02x idx=[%d %d %d] "
                   "a=(%.1f %.1f %.1f w%.1f) b=(%.1f %.1f %.1f w%.1f) c=(%.1f %.1f %.1f w%.1f) "
                   "uv0=(%d,%d) cross=%.2f", i, t->flags, i0, i1, i2,
                   a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, c.x, c.y, c.z, c.w,
                   (int)(int16_t)t->uv0.u, (int)(int16_t)t->uv0.v, dkr_ndc_cross(&a, &b, &c));
        }

        /* Near-plane clip FIRST, so everything downstream has a positive w and
         * the perspective divide is always well defined. A triangle straddling
         * the plane becomes a quad; one entirely behind it disappears here (that
         * is the plane doing its job, not a cull). */
        {
            struct LoadedVertex tri[3];
            struct LoadedVertex poly[DKR_CLIP_MAX_VERTS];
            int pn, k;

            tri[0] = a; tri[1] = b; tri[2] = c;
            pn = dkr_clip_near(tri, 3, poly);
            if (pn < 3) {
                dkr_dbg_clip_dropped++;
                continue;
            }
            if (pn > 3) {
                dkr_dbg_clipped++;
            }
            /* A sane projection makes z+w>=0 imply w>0 (see dkr_clip_near); a
             * degenerate game matrix could still leave a non-positive w, which
             * would divide by ~0 downstream. Skip loudly-countably rather than
             * emit a wild vertex. */
            for (k = 0; k < pn; k++) {
                if (!(poly[k].w > 0.0f)) break;
            }
            if (k != pn && dkr_clip_mode_get() != 2) {
                dkr_dbg_clip_degen++;
                continue;
            }

            /* Backface cull unless the triangle opts out (BACKFACE_DRAW = 0x40).
             * Winding assumption matches Emill fast3d G_CULL_BACK (cross < 0 =
             * back-facing); flip if front faces vanish (see report). Tested on
             * the CLIPPED polygon's total signed area: clipping preserves
             * winding, so a fully-inside triangle gives exactly the old
             * dkr_ndc_cross(a,b,c) decision, while a straddling one is now
             * decided from real on-screen geometry instead of being skipped.
             *
             * The cull SENSE is deliberately independent of both
             * rsp.geometry_mode's G_CULL_FRONT bit and rdp.viewport_flip_x, and
             * that is only correct because the two always move together.
             * On hardware the RSP culls in SCREEN space, after the viewport
             * transform, so a negative vscale[0] inverts every winding; the
             * effective rule is (viewport_flip XOR G_CULL_FRONT). DKR sets both
             * of those from the same condition and nothing else:
             * camera.c:2102 makes vscale[0] negative exactly when
             * CHEAT_MIRRORED_TRACKS is active, and tracks.c:1313/1362 is the
             * only gSPSetGeometryMode(G_CULL_FRONT) in the game, emitted under
             * the same `flip`. dRspInit clears both cull bits and G_CULL_BACK is
             * never set again, so the flag pair is the whole story.
             * Here the cross product is taken BEFORE dkr_emit_tri applies the
             * x-flip, which cancels the same inversion — the two double
             * negations agree.
             * Measured, not assumed: a 27-run route sweep (intro/hubs/menus/
             * attract/credits/Taj/2P-3P-4P/time-trial plus a real Adventure Two
             * mirrored race on track 5) binned all 20,570,011 gSPPolygon calls by
             * [viewport_flip_x][G_CULL_FRONT]: 20,082,334 in [0][0], 487,677 in
             * [1][1] (the mirrored race), and ZERO in the disagreeing bins
             * [0][1] and [1][0].
             * If a future path ever separates them, this test has to become
             * `cull_negative = (flip != cull_front)`. */
            if (!(t->flags & BACKFACE_DRAW)) {
                float area = 0.0f;
                for (k = 1; k + 1 < pn; k++) {
                    area += dkr_ndc_cross(&poly[0], &poly[k], &poly[k + 1]);
                }
                if (area < 0.0f) {
                    culled++;
                    continue;
                }
            }
            for (k = 1; k + 1 < pn; k++) {
                dkr_emit_tri(&poly[0], &poly[k], &poly[k + 1]);
                emitted++;
            }
        }
    }
    if (dkr_trace_this_frame)
        DTRACE("  polygon: emitted=%d culled=%d oob=%d of %d", emitted, culled, oob, num_tris);
    gfx_flush();
}

/* ------------------------------------------------------------------------- */
/* SP: matrices, moveword, geometry mode, texture, viewport                  */
/* ------------------------------------------------------------------------- */

static void dkr_load_identity(int slot) {
    float (*mm)[4] = rsp.mtx[slot];
    memset(mm, 0, sizeof(rsp.mtx[slot]));
    mm[0][0] = mm[1][1] = mm[2][2] = mm[3][3] = 1.0f;
}

/*
 * Decode a validated s15.16 split-format Mtx into an rsp slot. Split out of
 * dkr_load_matrix so the presentation-replay path can decode a matrix it just
 * recomposed on the stack, which by construction has no arena provenance and
 * would be rejected by that function's pointer checks.
 */
static void dkr_decode_matrix(int slot, const int32_t *a) {
    /* Every rsp matrix array is sized for the three G_MTX_DKR_INDEX_0..2 slots
     * (f3ddkr.h); the two-bit encoded index can name a fourth. */
    if (slot < 0 || slot > 2) return;
    float (*mm)[4] = rsp.mtx[slot];
    uint32_t orbits = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = a[i * 2 + j / 2];
            uint32_t frac_part = (uint32_t)a[8 + i * 2 + j / 2];
            orbits |= (uint32_t)int_part | frac_part;
            mm[i][j]     = (int32_t)(((uint32_t)int_part & 0xffff0000u) | (frac_part >> 16)) / 65536.0f;
            mm[i][j + 1] = (int32_t)(((uint32_t)int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
    /* An all-zero split matrix is not a real transform — it collapses every
     * vertex to (0,0,0,0) (w==0), so the whole draw disappears. DKR's matrix
     * pool has unfilled slots on some frames (a game-side pool-management gap
     * under LP64); treat a zero matrix as identity so geometry that carries its
     * own coordinates still renders, matching the previous NULL-skip default. */
    if (orbits == 0) dkr_load_identity(slot);
}

static void dkr_load_matrix(int slot, const void *addr) {
    if (slot < 0 || slot > 2) return;
    if (!addr || dkr_arena_room(addr) < sizeof(Mtx)) { dkr_load_identity(slot); return; }
    /* Belt-and-suspenders: a non-arena matrix pointer (room == SIZE_MAX) must be
     * a plausible registered global; never deref a sign-extended wild pointer. */
    if (dkr_arena_room(addr) == (size_t)-1 && !dkr_ptr_plausible(addr)) {
        dkr_load_identity(slot); return;
    }
    /* Standard N64 s15.16 split-format Mtx: 8 int words then 8 frac words, each
     * packing two elements' halves (ported from mgb64 gfx_sp_matrix). */
    dkr_decode_matrix(slot, (const int32_t *)addr);
}

static void dkr_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    (void)offset;
    switch (index) {
        case G_MW_BILLBOARD:    /* 0x02 (DKR) — 1 = enable, 0 = disable */
            rsp.billboard = (data != 0);
            break;
        case G_MW_MVPMATRIX:    /* 0x0A (DKR) — select active matrix slot */
            /* gSPSelectMatrixDKR(num) packs num at bit 6 (f3ddkr.h), so the
             * two-bit field can name a fourth slot that F3DDKR does not have.
             * Unreachable: every call site passes a G_MTX_DKR_INDEX_0..2
             * constant, and the two that pass a variable (camera.c:2465 and
             * :2570) assign it INDEX_0 or INDEX_1 on the line before. A
             * 27-run DL sweep saw slot 3 zero times. */
            rsp.active_slot = (data >> 6) & 3;
            if (rsp.active_slot > 2) rsp.active_slot = 0;
            break;
        case G_MW_DKR_REMASTER_TARGET: {
            uint8_t light_class = (uint8_t)(data >> 30);
            float direction[3];
            rsp.remaster_light_class = 0;
            rsp.smooth_normals = NULL;
            rsp.light_direction[0] = 0.0f;
            rsp.light_direction[1] = 1.0f;
            rsp.light_direction[2] = 0.0f;
            if ((light_class == 1 || light_class == 2) &&
                gfx_level_lighting_unpack_direction(
                    data & 0x3fffffffu, direction)) {
                rsp.remaster_light_class = light_class;
                memcpy(rsp.light_direction, direction,
                       sizeof(rsp.light_direction));
            }
            break;
        }
        case G_MW_DKR_SMOOTH_NORMALS:
            rsp.smooth_normals =
                data != 0
                    ? (const Vec3s *)dkr_resolve(data)
                    : NULL;
            if (data != 0 && rsp.smooth_normals == NULL) {
                gfx_dkr_remaster_missing_normal_batches++;
            }
            break;
        case G_MW_DKR_WORLD_REGION: {
            bool safe_region =
                data != 0 && !dkr_test_framed_world_unsafe_enabled();
            if (rsp.world_safe_region != safe_region) {
                rsp.world_safe_region = safe_region;
                dkr_remap_viewport_and_scissor();
            }
            break;
        }
        case G_MW_FOG:          /* 0x08 — fog_mul (hi 16) / fog_offset (lo 16) */
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)(data & 0xffff);
            break;
        case G_MW_SEGMENT: {    /* 0x06 — set an RSP segment base */
            uint32_t seg = (offset / 4) & 0xf;
            gfx_segment_table[seg] = (uintptr_t)dkr_resolve(data);
            break;
        }
        default:
            /*
             * G_MW_CLIP (0x04), G_MW_PERSPNORM, G_MW_NUMLIGHT, ... — no-op.
             *
             * G_MW_CLIP is the only one of these DKR actually emits: a
             * 27-run route sweep binned moveword indices and saw exactly
             * {02,04,06,08,0a,0c,0e,10}, with 0x04's count always four times
             * the G_TEXTURE count — i.e. the four words of a single
             * gSPClipRatio, from the same rcp_dkr.c dRspInit that carries the
             * lone gsSPTexture. That call is a literal gsSPClipRatio
             * (FRUSTRATIO_2) and there is no other clip-ratio emitter, so the
             * ratio is a compile-time constant of this ROM.
             * dkr_update_clip_expansion() consumes it as that constant (its
             * half-viewport overhang cap IS ratio 2). Should a second emitter
             * ever appear, that cap must be driven from here instead.
             */
            break;
    }
}

static MdkrDisplayRect dkr_draw_region(uint8_t draw_space) {
    const uint32_t width = dkr_output_overlay_active
        ? gfx_output_dimensions.width
        : gfx_current_dimensions.width;
    const uint32_t height = dkr_output_overlay_active
        ? gfx_output_dimensions.height
        : gfx_current_dimensions.height;
    MdkrDisplayLayout layout = mdkr_display_calculate_layout(
        width, height, mdkr_display_widescreen_enabled(),
        mdkr_display_forced_aspect());
    switch (draw_space) {
        case G_MTX_DKR_SPACE_SAFE_2D:
            return layout.safe;
        case G_MTX_DKR_SPACE_FULLBLEED:
            return layout.fullbleed;
        case G_MTX_DKR_SPACE_WIDE_BG:
            return layout.presentation;
        case G_MTX_DKR_SPACE_WORLD:
        default:
            return rsp.world_safe_region ? layout.safe : layout.presentation;
    }
}

static void dkr_map_logical_rect(const struct FloatXYWidthHeight *logical,
                                 uint8_t draw_space,
                                 bool clamp_to_drawable,
                                 struct XYWidthHeight *mapped) {
    MdkrDisplayRect region = dkr_draw_region(draw_space);
    float scale_x = region.width / dkr_logical_width;
    float scale_y = region.height / dkr_logical_height;
    float lx0 = logical->x;
    float ly0 = logical->y;
    float lx1 = logical->x + logical->width;
    float ly1 = logical->y + logical->height;

    if (clamp_to_drawable) {
        /*
         * Scissors only. DKR states its full-surface clip as
         * gDPSetScissor(0, 0, 0, w - 1, h - 1) -- rcp_dkr.c flags the "- 1" as
         * an unnecessary fill-mode habit. On a 320-column framebuffer that
         * discards one column; scaled to the host it discards
         * region.width / 320 columns, which at 1920 is a hard-edged six-pixel
         * band of stale pixels down the right screen edge.
         *
         * A rectangle that reaches every surface bound to within one logical
         * pixel is the game saying "do not clip" -- camera.c's DEFAULT_VIEWPORT
         * and its width-1/height-1 clamps are written the same way -- so that
         * rectangle is snapped out to the region bounds. The test deliberately
         * requires ALL FOUR edges: a split-screen or menu sub-rectangle that
         * happens to touch one bound keeps its authored inset exactly.
         */
        if (lx0 <= 1.0f && ly0 <= 1.0f &&
            lx1 >= dkr_logical_width - 1.0f &&
            ly1 >= dkr_logical_height - 1.0f) {
            lx0 = 0.0f;
            ly0 = 0.0f;
            lx1 = dkr_logical_width;
            ly1 = dkr_logical_height;
        }
    }

    float x0 = region.x + lx0 * scale_x;
    float y0 = region.y + ly0 * scale_y;
    float x1 = region.x + lx1 * scale_x;
    float y1 = region.y + ly1 * scale_y;
    int32_t ix0 = (int32_t)lroundf(x0);
    int32_t iy0 = (int32_t)lroundf(y0);
    int32_t ix1 = (int32_t)lroundf(x1);
    int32_t iy1 = (int32_t)lroundf(y1);

    if (clamp_to_drawable) {
        int32_t drawable_width = (int32_t)(dkr_output_overlay_active
            ? gfx_output_dimensions.width
            : gfx_current_dimensions.width);
        int32_t drawable_height = (int32_t)(dkr_output_overlay_active
            ? gfx_output_dimensions.height
            : gfx_current_dimensions.height);
        if (ix0 < 0) ix0 = 0;
        if (iy0 < 0) iy0 = 0;
        if (ix1 < 0) ix1 = 0;
        if (iy1 < 0) iy1 = 0;
        if (ix0 > drawable_width) ix0 = drawable_width;
        if (ix1 > drawable_width) ix1 = drawable_width;
        if (iy0 > drawable_height) iy0 = drawable_height;
        if (iy1 > drawable_height) iy1 = drawable_height;
        if (ix1 < ix0) ix1 = ix0;
        if (iy1 < iy0) iy1 = iy0;
    }

    mapped->x = ix0;
    mapped->y = iy0;
    mapped->width = ix1 - ix0;
    mapped->height = iy1 - iy0;
}

static void dkr_remap_viewport_and_scissor(void) {
    if (rdp.logical_viewport_valid) {
        dkr_map_logical_rect(&rdp.logical_viewport, rsp.draw_space, false, &rdp.viewport);
    }
    if (rdp.logical_scissor_valid) {
        dkr_map_logical_rect(&rdp.logical_scissor, rsp.draw_space, true, &rdp.scissor);
    }
    rdp.viewport_or_scissor_changed = true;
}

static void dkr_set_draw_space(uint8_t draw_space) {
    if (draw_space < G_MTX_DKR_SPACE_WORLD ||
        draw_space > G_MTX_DKR_SPACE_WIDE_BG ||
        rsp.draw_space == draw_space) {
        return;
    }
    rsp.draw_space = draw_space;
    dkr_remap_viewport_and_scissor();
}

static void dkr_prepare_draw_target(void) {
    if (dkr_primitive_prepared) {
        return;
    }
    dkr_primitive_prepared = true;
    if (dkr_output_overlay_active) {
        dkr_output_overlay_frame_draws++;
        if (!dkr_primitive_overlay_candidate) {
            /*
             * The authored corpus is scene then overlay. If a future list
             * returns to WORLD, preserve command order by drawing it into the
             * already-open output pass and report the contract violation.
             * Dropping the batch or drawing back into the completed scene
             * would both be visibly worse and would lose ordering.
             */
            dkr_output_overlay_late_world_draws++;
        }
        return;
    }
    if (dkr_output_overlay_suppressed ||
        !dkr_primitive_overlay_candidate ||
        dkr_primitive_serial <= dkr_last_world_primitive ||
        !dkr_native_ui_enabled() ||
        gfx_rapi == NULL ||
        gfx_rapi->begin_output_overlay == NULL ||
        (!g_pcRemasterFX &&
         gfx_current_dimensions.width == gfx_output_dimensions.width &&
         gfx_current_dimensions.height == gfx_output_dimensions.height)) {
        return;
    }

    /*
     * Flush before the target change. rdp already contains the new logical
     * viewport, but the backend still holds the viewport used by the buffered
     * world batch; dkr_setup_draw_state applies the remapped output viewport
     * only after this function returns.
     */
    gfx_flush();
    if (!gfx_rapi->begin_output_overlay()) {
        dkr_output_overlay_begin_failures++;
        return;
    }
    dkr_output_overlay_active = true;
    dkr_output_overlay_frames++;
    dkr_output_overlay_frame_draws = 1;
    dkr_remap_viewport_and_scissor();
}

static void dkr_calc_viewport(const Vp_t *vp) {
    float lw = dkr_logical_width, lh = dkr_logical_height;
    float width  = 2.0f * vp->vscale[0] / 4.0f;
    float height = 2.0f * vp->vscale[1] / 4.0f;
    float viewport_width = fabsf(width);

    /*
     * A gameplay viewport command starts a world pass. mtx_ortho follows its own
     * viewport command immediately and retags that retained logical rectangle as
     * SAFE_2D/FULLBLEED, while a subsequent model matrix inherits the tag.
     */
    rsp.draw_space = G_MTX_DKR_SPACE_WORLD;
    rdp.viewport_flip_x = width < 0.0f;
#ifdef NATIVE_PORT
    if (rdp.viewport_flip_x && mdkr_trace_enabled()) {
        static bool reported_mirrored_viewport;
        if (!reported_mirrored_viewport) {
            mdkr_trace("adventure_viewport: n64Width=%.1f clipXFlip=1 hostWidth=%.1f",
                       width, viewport_width);
            reported_mirrored_viewport = true;
        }
    }
#endif
    rdp.logical_viewport.x =
        (vp->vtrans[0] / 4.0f) - viewport_width / 2.0f;
    rdp.logical_viewport.y =
        lh - ((vp->vtrans[1] / 4.0f) + height / 2.0f);
    rdp.logical_viewport.width = viewport_width;
    rdp.logical_viewport.height = height;
    rdp.logical_viewport_valid = true;
    dkr_remap_viewport_and_scissor();
}

/* ------------------------------------------------------------------------- */
/* RDP: tiles, textures, TLUT                                                */
/* ------------------------------------------------------------------------- */

static void dkr_dp_set_texture_image(uint32_t siz, uint32_t width, const void *addr) {
    rdp.to_load.addr = (const uint8_t *)addr;
    rdp.to_load.siz = (uint8_t)siz;
    rdp.to_load.width = width;
}

static void dkr_dp_set_tile(uint8_t fmt, uint8_t siz, uint32_t line, uint32_t tmem,
                            uint8_t tile, uint32_t palette, uint32_t cmt, uint32_t maskt,
                            uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    if (tile >= 8) return;
    rdp.tile[tile].fmt = fmt;
    rdp.tile[tile].siz = siz;
    rdp.tile[tile].line_size_bytes = line * 8;
    rdp.tile[tile].tmem = (uint16_t)tmem;
    rdp.tile[tile].palette = (uint8_t)palette;
    rdp.tile[tile].cmt = (uint8_t)cmt;
    rdp.tile[tile].maskt = (uint8_t)maskt;
    rdp.tile[tile].shiftt = (uint8_t)shiftt;
    rdp.tile[tile].cms = (uint8_t)cms;
    rdp.tile[tile].masks = (uint8_t)masks;
    rdp.tile[tile].shifts = (uint8_t)shifts;
}

static void dkr_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult,
                                 uint16_t lrs, uint16_t lrt) {
    if (tile >= 8) return;
    rdp.tile[tile].uls = uls;
    rdp.tile[tile].ult = ult;
    rdp.tile[tile].lrs = lrs;
    rdp.tile[tile].lrt = lrt;
    int32_t sd = (int32_t)lrs - (int32_t)uls;
    int32_t td = (int32_t)lrt - (int32_t)ult;
    rdp.tile[tile].width  = sd >= 0 ? (uint16_t)((sd >> 2) + 1) : 0;
    rdp.tile[tile].height = td >= 0 ? (uint16_t)((td >> 2) + 1) : 0;
}

static void dkr_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult,
                              uint32_t lrs, uint32_t dxt) {
    (void)uls; (void)ult;
    uint32_t shift;
    switch (rdp.to_load.siz) {
        case G_IM_SIZ_16b: shift = 1; break;
        case G_IM_SIZ_32b: shift = 2; break;
        default:           shift = 0; break;
    }
    uint32_t size_bytes = (lrs + 1) << shift;
    uint32_t slot = (tile < 8) ? rdp.tile[tile].tmem : 0;
    if (slot >= 512) slot = 0;
    rdp.loaded_texture[slot].addr = rdp.to_load.addr;
    rdp.loaded_texture[slot].size_bytes = size_bytes;
    rdp.loaded_texture[slot].line_swapped = (dxt == 0);
    if (!dkr_replay_pass && rdp.to_load.addr != NULL && size_bytes != 0u) {
        (void)gfx_retained_task_capture_dependency(
            rdp.to_load.addr, rdp.to_load.addr, size_bytes);
    }
}

static void dkr_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult,
                             uint32_t lrs, uint32_t lrt) {
    if (tile >= 8) return;
    uint32_t shift;
    switch (rdp.to_load.siz) {
        case G_IM_SIZ_16b: shift = 1; break;
        case G_IM_SIZ_32b: shift = 2; break;
        default:           shift = 0; break;
    }
    int32_t sd = (int32_t)lrs - (int32_t)uls;
    int32_t td = (int32_t)lrt - (int32_t)ult;
    if (sd < 0 || td < 0) return;
    uint32_t width  = (uint32_t)(sd >> 2) + 1;
    uint32_t height = (uint32_t)(td >> 2) + 1;
    uint32_t slot = rdp.tile[tile].tmem < 512 ? rdp.tile[tile].tmem : 0;
    uint32_t line = rdp.tile[tile].line_size_bytes;
    if (line == 0) line = (width << shift);
    rdp.loaded_texture[slot].addr = rdp.to_load.addr;
    rdp.loaded_texture[slot].size_bytes = line * height;
    /* LOADTILE walks the source row by row, so DRAM order is plain linear. */
    rdp.loaded_texture[slot].line_swapped = false;
    rdp.tile[tile].width = (uint16_t)width;
    rdp.tile[tile].height = (uint16_t)height;
    if (!dkr_replay_pass && rdp.to_load.addr != NULL &&
        rdp.loaded_texture[slot].size_bytes != 0u) {
        (void)gfx_retained_task_capture_dependency(
            rdp.to_load.addr, rdp.to_load.addr,
            rdp.loaded_texture[slot].size_bytes);
    }
}

static void dkr_dp_load_tlut(uint8_t tile, uint32_t uls, uint32_t ult,
                             uint32_t lrs, uint32_t lrt) {
    if (tile >= 8) return;
    (void)ult; (void)lrt;
    if (lrs < uls) return;
    /* TLUT load index range is in tile 10.2 units → colours = span/4 + 1. */
    uint32_t count = ((lrs >> 2) - (uls >> 2)) + 1;
    uint32_t palofs = rdp.tile[tile].tmem >= 256 ? rdp.tile[tile].tmem - 256 : 0;
    if (palofs >= 256) palofs = 0;
    if (count > 256 - palofs) count = 256 - palofs;
    /* TLUT entries are big-endian ROM bytes (like texels); read MSB-first. */
    const uint8_t *src = (const uint8_t *)rdp.to_load.addr;
    if (!src) return;
    /* Never read the TLUT past the arena: the entry count comes from the display
     * list, the load address from the game. */
    {
        size_t room = dkr_arena_room(src);
        if (room == (size_t)-1) {
            if (!dkr_ptr_plausible(src)) return;
        } else if ((size_t)count * sizeof(uint16_t) > room) {
            count = (uint32_t)(room / sizeof(uint16_t));
        }
    }
    if (!dkr_replay_pass && count != 0u) {
        (void)gfx_retained_task_capture_dependency(
            src, src, (size_t)count * sizeof(uint16_t));
    }
    for (uint32_t i = 0; i < count; i++)
        rdp.palette[palofs + i] = ((uint16_t)src[i * 2] << 8) | src[i * 2 + 1];
    rdp.palette_fmt = (rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT));
}

/* ------------------------------------------------------------------------- */
/* RDP: combine, colours, images, scissor                                    */
/* ------------------------------------------------------------------------- */

static void dkr_dp_set_combine(const Gfx *cmd) {
    /* Normalize the two SETCOMBINE words into the 28-bit-per-cycle cc_id used
     * by dkr_generate_cc (identical field layout to mgb64). */
    uint32_t rgb  = color_comb(C0(cmd,20,4), C1(cmd,28,4), C0(cmd,15,5), C1(cmd,15,3));
    uint32_t a    = alpha_comb(C0(cmd,12,3), C1(cmd,12,3), C0(cmd,9,3),  C1(cmd,9,3));
    uint32_t rgb2 = color_comb(C0(cmd,5,4),  C1(cmd,24,4), C0(cmd,0,5),  C1(cmd,6,3));
    uint32_t a2   = alpha_comb(C1(cmd,21,3), C1(cmd,3,3),  C1(cmd,18,3), C1(cmd,0,3));
    rdp.combine_mode = (uint64_t)rgb | ((uint64_t)a << 16) |
                       ((uint64_t)rgb2 << 28) | ((uint64_t)a2 << 44);
}

static void dkr_dp_set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    /* 10.2 fixed-point -> 320x240 bottom-left logical coordinates. Retain the
     * source rectangle because the following tagged matrix selects whether this
     * is a world, safe-area or full-bleed pass. */
    float x = ulx / 4.0f;
    float y_top = uly / 4.0f;
    float width = ((int32_t)lrx - (int32_t)ulx) / 4.0f;
    float height = ((int32_t)lry - (int32_t)uly) / 4.0f;
    rdp.logical_scissor.x = x;
    rdp.logical_scissor.y = dkr_logical_height - (y_top + height);
    rdp.logical_scissor.width = width;
    rdp.logical_scissor.height = height;
    rdp.logical_scissor_valid = true;
    dkr_map_logical_rect(&rdp.logical_scissor, rsp.draw_space, true, &rdp.scissor);
    rdp.viewport_or_scissor_changed = true;
}

/* ------------------------------------------------------------------------- */
/* Screen-space rectangles (FILLRECT / TEXRECT / TEXRECTFLIP)                */
/* ------------------------------------------------------------------------- */

/* Two triangles filling a quad in NDC; viewport/scissor temporarily set to the
 * full drawable (ported from mgb64 gfx_draw_rectangle). */
static void dkr_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry,
                               bool textured, bool poly_tex,
                               float s0, float t0, float s1, float t1) {
    dkr_prepare_draw_target();
    MdkrDisplayRect region = dkr_draw_region(rsp.draw_space);
    float drawable_width = (float)(dkr_output_overlay_active
        ? gfx_output_dimensions.width
        : gfx_current_dimensions.width);
    float drawable_height = (float)(dkr_output_overlay_active
        ? gfx_output_dimensions.height
        : gfx_current_dimensions.height);
    float scale_x = region.width / dkr_logical_width;
    float scale_y = region.height / dkr_logical_height;
    float ulx_pixels = region.x + (ulx / 4.0f) * scale_x;
    float lrx_pixels = region.x + (lrx / 4.0f) * scale_x;
    /* Region coordinates are bottom-left; RDP rectangle Y is top-left. */
    float uly_pixels = region.y + region.height - (uly / 4.0f) * scale_y;
    float lry_pixels = region.y + region.height - (lry / 4.0f) * scale_y;
    float ulxf = (2.0f * ulx_pixels / drawable_width) - 1.0f;
    float ulyf = (2.0f * uly_pixels / drawable_height) - 1.0f;
    float lrxf = (2.0f * lrx_pixels / drawable_width) - 1.0f;
    float lryf = (2.0f * lry_pixels / drawable_height) - 1.0f;

    struct LoadedVertex *ul = &rsp.loaded[DKR_MAX_VERTICES + 0];
    struct LoadedVertex *ll = &rsp.loaded[DKR_MAX_VERTICES + 1];
    struct LoadedVertex *lr = &rsp.loaded[DKR_MAX_VERTICES + 2];
    struct LoadedVertex *ur = &rsp.loaded[DKR_MAX_VERTICES + 3];

    ul->x = ulxf; ul->y = ulyf; ul->z = 0; ul->w = 1; ul->u = s0; ul->v = t0;
    ll->x = ulxf; ll->y = lryf; ll->z = 0; ll->w = 1; ll->u = s0; ll->v = t1;
    lr->x = lrxf; lr->y = lryf; lr->z = 0; lr->w = 1; lr->u = s1; lr->v = t1;
    ur->x = lrxf; ur->y = ulyf; ur->z = 0; ur->w = 1; ur->u = s1; ur->v = t0;
    ul->color = ll->color = lr->color = ur->color = rdp.prim_color;
    ul->fog = ll->fog = lr->fog = ur->fog = 0;

    struct XYWidthHeight vp_saved = rdp.viewport, sc_saved = rdp.scissor;
    uint32_t gm_saved = rsp.geometry_mode;
    struct XYWidthHeight full = {
        0, 0,
        (int32_t)(dkr_output_overlay_active
            ? gfx_output_dimensions.width
            : gfx_current_dimensions.width),
        (int32_t)(dkr_output_overlay_active
            ? gfx_output_dimensions.height
            : gfx_current_dimensions.height)
    };
    rdp.viewport = full;
    rdp.scissor = full;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;

    dkr_in_texrect = textured;   /* absolute texel coords: skip NOPERSP *0.5 */
    /* Same rule as dkr_sp_polygon: a rectangle whose texture failed to bind
     * would show the previous draw's image at full screen-space size. Restore
     * the saved RSP/RDP state below either way. */
    if (dkr_setup_draw_state(textured && poly_tex)) {
        if (!textured) cur.use_texture = false;
        dkr_emit_tri(ul, ll, ur);
        dkr_emit_tri(ll, lr, ur);
        gfx_flush();
    }
    dkr_in_texrect = false;

    rsp.geometry_mode = gm_saved;
    rdp.viewport = vp_saved;
    rdp.scissor = sc_saved;
    rdp.viewport_or_scissor_changed = true;
}

/* Resolve the flat colour a FILL_RECTANGLE takes when the RDP is NOT in
 * G_CYC_FILL/G_CYC_COPY.
 *
 * Outside fill/copy mode a FILL_RECTANGLE is rasterized as an ordinary
 * screen-space rectangle: the colour combiner still runs, but the span carries
 * no texel and no shade coefficients, so only the combiner's "d" term is
 * meaningful -- (a - b) * c + d degenerates to d. DKR depends on exactly that
 * for every dialogue-box background: render_dialogue_box() selects
 * G_CC_ENVIRONMENT (0,0,0,ENVIRONMENT / 0,0,0,ENVIRONMENT) and sets the colour
 * with gDPSetEnvColor, then issues plain FILLRECTs in 1-cycle mode.
 *
 * Sourcing the colour from prim_color unconditionally therefore drew those
 * panels in whatever prim_color happened to be -- which on this path is
 * (0,0,0,0), i.e. fully transparent, so the panel vanished. Read the register
 * the combiner actually names instead. */
static struct RGBA dkr_fillrect_flat_color(void) {
    uint32_t rgb_d = (uint32_t)((rdp.combine_mode >> 13) & 7);
    uint32_t alp_d = (uint32_t)((rdp.combine_mode >> 25) & 7);
    struct RGBA out = rdp.prim_color;

    switch (rgb_d) {
        case G_CCMUX_ENVIRONMENT:
            out.r = rdp.env_color.r; out.g = rdp.env_color.g; out.b = rdp.env_color.b; break;
        case G_CCMUX_1:
            out.r = out.g = out.b = 255; break;
        case (G_CCMUX_0 & 7): /* G_CCMUX_0 is 31; color_comb() keeps only 3 bits */
            out.r = out.g = out.b = 0; break;
        default: /* PRIMITIVE, or a texel/shade/combined term a flat fill cannot
                  * supply -- keep prim_color, the historical fallback. */
            break;
    }
    switch (alp_d) {
        case G_ACMUX_ENVIRONMENT: out.a = rdp.env_color.a; break;
        case G_ACMUX_1:           out.a = 255; break;
        case G_ACMUX_0:           out.a = 0;   break;
        default:                  break;
    }
    return out;
}

static void dkr_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    /* Skip z-buffer clears (colour image aimed at the depth image). Compare the
     * RAW SETCIMG/SETZIMG tokens rather than the resolved pointers: DKR clears
     * the depth buffer by pointing SETCIMG at the z-buffer segment (so both
     * tokens are e.g. 0x02000000), and on wasm32 those segment tokens resolve to
     * NULL — a resolved-pointer compare would then draw the clear as a white
     * fill over the scene (M3b). Tokens are width-independent and identical on
     * native (0x01000000 framebuffer vs 0x02000000 z-buffer for a real fill). */
    if (rdp.color_image_token != 0 &&
        rdp.color_image_token == rdp.z_buf_token) return;
    uint32_t cyc = rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE);
    if (cyc == G_CYC_FILL || cyc == G_CYC_COPY) { lrx += 1 << 2; lry += 1 << 2; }
    uint8_t saved_space = rsp.draw_space;
    bool promoted_to_fullbleed = false;
    /*
     * The framebuffer clear arrives before a camera matrix and must paint the
     * host gutters too.  Restrict the promotion to a WORLD fill/copy rectangle
     * that covers the complete logical framebuffer; full-screen menu panels
     * drawn after SAFE_2D tagging remain undistorted in the safe area.
     */
    if (saved_space == G_MTX_DKR_SPACE_WORLD &&
        (cyc == G_CYC_FILL || cyc == G_CYC_COPY) &&
        ulx <= 0 && uly <= 0 &&
        lrx >= DESIRED_SCREEN_WIDTH * 4 &&
        lry >= DESIRED_SCREEN_HEIGHT * 4) {
        /*
         * Change the space through the normal setter, not just the tag: a
         * forced-aspect presentation can have a narrower world scissor. The
         * full clear must remap that scissor too or its pillarbox gutters retain
         * pixels from the previous frame.
         */
        dkr_set_draw_space(G_MTX_DKR_SPACE_FULLBLEED);
        /*
         * This is a scene clear promoted only so it covers widescreen gutters;
         * it is not an authored transition overlay and must not resolve the
         * scene before the camera/world commands that follow.
         */
        dkr_output_overlay_suppressed = true;
        promoted_to_fullbleed = true;
    }
    struct RGBA rc = (cyc == G_CYC_FILL || cyc == G_CYC_COPY) ? rdp.fill_color
                                                              : dkr_fillrect_flat_color();
    struct RGBA saved = rdp.prim_color;
    rdp.prim_color = rc;
    /* Fill uses a flat prim/fill colour; force a trivial shade combiner so the
     * generated shader outputs prim colour regardless of the live combiner. */
    uint64_t saved_cc = rdp.combine_mode;
    rdp.combine_mode = (uint64_t)color_comb(0, 0, 0, G_CCMUX_SHADE) |
                       ((uint64_t)alpha_comb(0, 0, 0, G_ACMUX_SHADE) << 16);
    dkr_draw_rectangle(ulx, uly, lrx, lry, false, false, 0, 0, 0, 0);
    rdp.combine_mode = saved_cc;
    rdp.prim_color = saved;
    if (promoted_to_fullbleed) {
        dkr_output_overlay_suppressed = false;
        dkr_set_draw_space(saved_space);
    }
}

static void dkr_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry,
                                     uint8_t tile, int16_t uls, int16_t ult,
                                     int16_t dsdx, int16_t dtdy, bool flip) {
    uint32_t cyc = rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE);
    if (cyc == G_CYC_COPY) { dsdx >>= 2; lrx += 1 << 2; lry += 1 << 2; }

    uint8_t tb_saved = rsp.tile_base;
    rsp.tile_base = tile;

    int32_t draw_ulx = (ulx >> 2) << 2;
    int32_t draw_uly = (uly >> 2) << 2;
    int32_t draw_lrx = ((lrx + 3) >> 2) << 2;
    int32_t draw_lry = ((lry + 3) >> 2) << 2;
    float wpix = (draw_lrx - draw_ulx) / 4.0f;
    float hpix = (draw_lry - draw_uly) / 4.0f;
    float s_ext = flip ? hpix : wpix;
    float t_ext = flip ? wpix : hpix;

    float uls_e = (float)uls, ult_e = (float)ult;
    float dsdx_55 = (float)dsdx / 32.0f;
    float dtdy_55 = (float)dtdy / 32.0f;
    if (dsdx < 0) uls_e -= dsdx_55;
    if (dtdy < 0) ult_e -= dtdy_55;
    float lrs = uls_e + dsdx_55 * s_ext;
    float lrt = ult_e + dtdy_55 * t_ext;

    /* UVs are stored in S10.5 (dkr_vbo_texcoord divides by 32).
     *
     * The G_TEXRECTFLIP transposition (S advancing with screen Y and T with
     * screen X) is NOT implemented: `flip` only swaps which screen extent scales
     * dsdx/dtdy above, and the emitted quad always maps S along X and T along Y.
     * A flipped rectangle would therefore draw with its texture un-transposed.
     *
     * Measured: a DL opcode census (MDKR_DL_CENSUS=1) over boot/attract and over
     * a full race route counted 101,362 G_TEXRECT (0xe4) commands and ZERO
     * G_TEXRECTFLIP (0xe5). No game source emits gSPTextureRectangleFlip either,
     * so no reachable DKR content exercises this path. */
    dkr_draw_rectangle(draw_ulx, draw_uly, draw_lrx, draw_lry, true, true,
                       uls_e, ult_e, lrs, lrt);
    rsp.tile_base = tb_saved;
}

/* ------------------------------------------------------------------------- */
/* Main command dispatch                                                     */
/* ------------------------------------------------------------------------- */

static uint64_t s_dl_census_opcodes[256];
static uint64_t s_dl_census_commands;
static uint64_t s_dl_census_lists;
static uint64_t s_dl_census_faults;
static unsigned s_dl_census_max_depth;
static bool s_dl_census_reported;
static int s_dl_census_active = -1;

static bool dkr_dl_census_enabled(void) {
    /* A replay walks a display list the census already counted. Counting it
     * twice would silently double every opcode total the census reports. */
    if (dkr_replay_pass) {
        return false;
    }
    if (s_dl_census_active < 0) {
        const char *value = getenv("MDKR_DL_CENSUS");
        s_dl_census_active = value != NULL && value[0] == '1';
    }
    return s_dl_census_active != 0;
}

/* Interpret a display list. `limit` bounds the number of 64-bit command slots
 * processed (used by G_DMADL, whose DMA sub-lists carry a command count and may
 * lack a G_ENDDL terminator); limit == 0 runs until G_ENDDL / list end. */
static void dkr_dl_fault(const char *reason, const Gfx *cmd, int depth) {
    static int strict = -1;
    if (strict < 0) {
        const char *strict_value = getenv("MDKR_DL_STRICT");
        const char *legacy_lenient = getenv("MDKR_DL_LENIENT");
        strict =
            strict_value != NULL && strict_value[0] == '1' &&
            !(legacy_lenient != NULL && legacy_lenient[0] == '1');
    }
    if (dkr_dl_census_enabled()) {
        s_dl_census_faults++;
    }
    fprintf(stderr,
            "[DL] %s at depth=%d cmd=%p words=%08x/%08x%s\n",
            reason, depth, (const void *) cmd,
            cmd != NULL ? cmd->words.w0 : 0,
            cmd != NULL ? cmd->words.w1 : 0,
            strict ? " (strict: aborting)" : " (recovered: list stopped/skipped)");
    fflush(stderr);
    if (strict) {
        abort();
    }
}

/*
 * Overlay-ordering prepass.
 *
 * DKR usually emits world geometry followed by HUD geometry, but level-entry
 * frames begin with an authored SAFE_2D loading mosaic and then return to the
 * world. A streaming renderer cannot discover that return after it has already
 * resolved the scene. This lightweight structural walk finds the final WORLD
 * primitive before the real interpreter runs; only non-world primitives after
 * that ordinal are eligible for the output-resolution pass.
 *
 * The walk follows G_DL/G_DMADL and the segment-table commands needed to
 * resolve them, but does not touch game/RSP/RDP state. Segment bases are
 * snapshotted and restored by the caller.
 */
typedef struct DkrOverlayScan {
    uint8_t draw_space;
    uint64_t primitive;
    uint64_t last_world;
} DkrOverlayScan;

static void dkr_scan_overlay_order(Gfx *cmd, int depth, int limit,
                                   DkrOverlayScan *scan) {
    Gfx *start;
    long safety = 0;

    if (cmd == NULL || scan == NULL || depth >= DKR_DL_MAX_DEPTH) {
        return;
    }
    start = cmd;
    for (;;) {
        if (limit > 0 && (cmd - start) >= limit) {
            return;
        }
        if (++safety > 4000000L) {
            return;
        }
        {
            uintptr_t uc = (uintptr_t)cmd;
            uintptr_t ab = (uintptr_t)g_dkrArenaBase;
            uintptr_t ae = ab + (uintptr_t)g_dkrArenaSize;
            if ((uc >= ae && uc < ae + 0x00010000u) ||
                dkr_arena_room(cmd) < sizeof(Gfx)) {
                return;
            }
        }

        uint8_t op = (uint8_t)C0(cmd, 24, 8);
        switch (op) {
            case G_DL: {
                uint8_t nopush = (uint8_t)C0(cmd, 16, 8);
                Gfx *sub = (Gfx *)dkr_resolve(cmd->words.w1);
                if (sub == NULL) {
                    if (nopush == G_DL_NOPUSH) {
                        return;
                    }
                    break;
                }
                if (nopush == G_DL_NOPUSH) {
                    cmd = sub;
                    start = cmd;
                    continue;
                }
                dkr_scan_overlay_order(sub, depth + 1, 0, scan);
                break;
            }
            case G_DMADL: {
                int count = (int)C0(cmd, 16, 8);
                Gfx *sub = (Gfx *)dkr_resolve(cmd->words.w1);
                if (sub != NULL && count > 0) {
                    dkr_scan_overlay_order(sub, depth + 1, count, scan);
                }
                break;
            }
            case (uint8_t)G_ENDDL:
                return;
            case G_MTX: {
                uint8_t draw_space = (uint8_t)C0(cmd, 16, 8) & 7;
                if (draw_space != G_MTX_DKR_SPACE_INHERIT) {
                    scan->draw_space = draw_space;
                }
                break;
            }
            case G_MOVEMEM:
                if ((uint8_t)C0(cmd, 16, 8) == G_MV_VIEWPORT) {
                    scan->draw_space = G_MTX_DKR_SPACE_WORLD;
                }
                break;
            case (uint8_t)G_MOVEWORD:
                if ((uint8_t)C0(cmd, 0, 8) == G_MW_SEGMENT) {
                    uint16_t offset = (uint16_t)C0(cmd, 8, 16);
                    uint32_t seg = (offset / 4) & 0xf;
                    gfx_segment_table[seg] =
                        (uintptr_t)dkr_resolve(cmd->words.w1);
                }
                break;
            case G_TRIN:
                if (dkr_resolve(cmd->words.w1) != NULL) {
                    scan->primitive++;
                    if (scan->draw_space == G_MTX_DKR_SPACE_WORLD) {
                        scan->last_world = scan->primitive;
                    }
                }
                break;
            case G_FILLRECT:
                scan->primitive++;
                /*
                 * A WORLD-tagged framebuffer clear is scene work. Other
                 * FILLRECTs are screen-space panels/transitions even when the
                 * game did not emit a fresh ortho matrix tag.
                 */
                if (scan->draw_space == G_MTX_DKR_SPACE_WORLD &&
                    (int32_t)C1(cmd, 12, 12) <= 0 &&
                    (int32_t)C1(cmd, 0, 12) <= 0 &&
                    (int32_t)C0(cmd, 12, 12) >=
                        DESIRED_SCREEN_WIDTH * 4 - 4 &&
                    (int32_t)C0(cmd, 0, 12) >=
                        DESIRED_SCREEN_HEIGHT * 4 - 4) {
                    scan->last_world = scan->primitive;
                }
                break;
            case G_TEXRECT:
            case G_TEXRECTFLIP:
                scan->primitive++;
                if ((limit > 0 && (cmd - start) + 2 >= limit) ||
                    dkr_arena_room(cmd) < sizeof(Gfx) * 3) {
                    return;
                }
                cmd += 2;
                break;
            default:
                break;
        }
        cmd++;
    }
}

/* ------------------------------------------------------------------------- */
/* Game-declared ping-pong triangle buffers                                    */
/* ------------------------------------------------------------------------- */

#define DKR_PAIRED_TRIANGLE_REGIONS 4u

static struct DkrPairedTriangleRegion {
    const uint8_t *base;
    size_t stride;
    unsigned count;
} dkr_paired_triangle_regions[DKR_PAIRED_TRIANGLE_REGIONS];

void gfx_dkr_note_paired_triangle_buffers(const void *base, size_t stride,
                                          unsigned count) {
    unsigned index;
    unsigned free_slot = DKR_PAIRED_TRIANGLE_REGIONS;

    if (base == NULL || stride == 0u || count < 2u) {
        return;
    }
    for (index = 0u; index < DKR_PAIRED_TRIANGLE_REGIONS; index++) {
        if (dkr_paired_triangle_regions[index].base == (const uint8_t *)base) {
            dkr_paired_triangle_regions[index].stride = stride;
            dkr_paired_triangle_regions[index].count = count;
            return;
        }
        if (dkr_paired_triangle_regions[index].base == NULL &&
            free_slot == DKR_PAIRED_TRIANGLE_REGIONS) {
            free_slot = index;
        }
    }
    if (free_slot == DKR_PAIRED_TRIANGLE_REGIONS) {
        return;                  /* full: those surfaces keep holding */
    }
    dkr_paired_triangle_regions[free_slot].base = (const uint8_t *)base;
    dkr_paired_triangle_regions[free_slot].stride = stride;
    dkr_paired_triangle_regions[free_slot].count = count;
}

void gfx_dkr_forget_paired_triangle_buffers(const void *base) {
    unsigned index;

    for (index = 0u; index < DKR_PAIRED_TRIANGLE_REGIONS; index++) {
        if (base == NULL ||
            dkr_paired_triangle_regions[index].base == (const uint8_t *)base) {
            memset(&dkr_paired_triangle_regions[index], 0,
                   sizeof(dkr_paired_triangle_regions[index]));
        }
    }
}

/*
 * Map an address inside a declared ping-pong region onto its partner phase and
 * onto the phase-invariant key both phases share.
 *
 * `*out_partner` is the same offset in the other buffer of the pair and
 * `*out_canonical` is the even-phase address, which is what the retained table
 * is keyed by so that the two ticks of one surface agree on one key.
 *
 * Returns false for every address outside a declared region -- the ordinary,
 * address-stable case, where the address is already its own identity.
 */
static bool dkr_paired_triangle_alias(const void *address,
                                      const void **out_partner,
                                      const void **out_canonical) {
    const uint8_t *bytes = (const uint8_t *)address;
    unsigned index;

    for (index = 0u; index < DKR_PAIRED_TRIANGLE_REGIONS; index++) {
        const struct DkrPairedTriangleRegion *region =
            &dkr_paired_triangle_regions[index];
        size_t span;
        size_t offset;
        size_t buffer;
        size_t within;
        if (region->base == NULL) {
            continue;
        }
        span = region->stride * (size_t)region->count;
        if (bytes < region->base || bytes >= region->base + span) {
            continue;
        }
        offset = (size_t)(bytes - region->base);
        buffer = offset / region->stride;
        within = offset % region->stride;
        /* Phase is bit 0 of the buffer index: waves.c selects
         * gWaveTriangles[flip + (k << 1)], so buffers 2k and 2k+1 are the two
         * phases of surface k. An odd `count` leaves a final unpaired buffer;
         * it has no partner and keeps holding. */
        if ((buffer ^ 1u) >= region->count) {
            return false;
        }
        if (out_partner != NULL) {
            *out_partner =
                region->base + (buffer ^ 1u) * region->stride + within;
        }
        if (out_canonical != NULL) {
            *out_canonical =
                region->base + (buffer & ~(size_t)1u) * region->stride + within;
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Authored UV scroll: retained {T, T+1} endpoints for one triangle batch      */
/* ------------------------------------------------------------------------- */
/*
 * WHAT SCROLLS, AND HOW IT TRAVELS.
 *
 * No DKR scroller uses G_SETTILESIZE, gSPTexture or a texture matrix. Every one
 * of them rewrites the S10.5 corner UVs inside a Triangle array IN PLACE, once
 * per authored tick, and gSPPolygon (G_TRIN) carries only a pointer to that
 * array. The drivers are obj_loop_texscroll (BHV_TEXTURE_SCROLL — the level-wide
 * waterfall/river/lava scroller, matched by texture index), obj_loop_animator
 * (BHV_ANIMATOR — one segment/batch), the generated wave surfaces in waves.c,
 * and the rain/particle/skydome sheets. They differ only in which triangles they
 * touch and in their wrap modulus.
 *
 * That is why this census does not need to know which driver owns a batch. The
 * retained task holds tick T's bytes for the array; the already-authored next
 * task names the same array now holding T+1. The difference of the two IS the
 * tick's displacement, whatever produced it.
 *
 * THE WRAP RULE.
 *
 * Each driver periodically folds a corner back into range by adding or
 * subtracting a whole number of texture repeats: texscroll uses width<<8 and
 * height<<8 (8 repeats), the animator width<<7 (4 repeats), waves width*32
 * (1 repeat), rain (width<<5)*2, the skydome width<<9, and the particle sheet a
 * flat 512. Every one of those moduli is a multiple of 32 — one texel — because
 * they are all whole repeats of a texture whose width is a whole number of
 * texels. A folded corner therefore reports a raw difference of D - k*M rather
 * than the true displacement D.
 *
 * Interpolating the RAW difference would sweep a scrolling surface through the
 * whole texture inside one tick. So the shortest wrap distance is recovered
 * structurally instead of numerically: every triangle in a batch receives the
 * SAME displacement, a batch is at most 16 triangles, and the folds are keyed
 * off each triangle's own first corner, so the un-folded triangles state D
 * directly. Small differences — at or below DKR_UV_SCROLL_MAX_DELTA, eight
 * texels a tick, well above any authored scroll (the fastest measured is four)
 * — are the candidates for D, and they must all agree; every larger difference
 * must then be D offset by an exact multiple of one texel.
 *
 * Everything else refuses. Two disagreeing small candidates is exactly what a
 * fold shorter than the limit produces, and it refuses the batch outright
 * rather than picking one. A batch where every triangle folded on the same tick
 * leaves no un-folded candidate at all, and the displacement it would report is
 * D minus the modulus; gfx_presentation_packet_lookup_uv_scroll is what keeps
 * that off the screen, because it requires the previous published tick to have
 * agreed on the same displacement. Authored scroll speed is a level constant,
 * so a real scroller confirms on its second tick and stays confirmed, while a
 * transient misread would have to repeat itself exactly to survive — which a
 * fold, by definition, does not do on consecutive ticks.
 */
#define DKR_UV_SCROLL_MAX_DELTA 255   /* S10.5 units; < one texel fold (32) x8 */
#define DKR_UV_SCROLL_FOLD_UNIT 32    /* one texel: every driver's modulus
                                       * is a whole number of these          */

/* Resolve one axis of a batch: `raw[i]` is triangle i's tick-over-tick corner
 * difference. Returns false when the batch cannot be explained as a single
 * displacement plus whole-texel folds. */
static bool dkr_uv_scroll_resolve_axis(const int32_t *raw, int num_tris,
                                       int32_t *out_delta,
                                       uint16_t *out_moved) {
    int32_t delta = 0;
    bool have_delta = false;
    uint16_t moved = 0u;
    int i;

    for (i = 0; i < num_tris; i++) {
        int32_t value = raw[i];
        if (value == 0 || value > DKR_UV_SCROLL_MAX_DELTA ||
            value < -DKR_UV_SCROLL_MAX_DELTA) {
            continue;
        }
        if (!have_delta) {
            delta = value;
            have_delta = true;
        } else if (value != delta) {
            /* Two different un-folded displacements in one batch: this is not
             * one scroller, or the batch was rebuilt. Refuse. */
            return false;
        }
    }
    if (!have_delta) {
        /* Either nothing moved (delta 0, handled by the caller) or every
         * triangle folded on this tick and no candidate is trustworthy. */
        *out_delta = 0;
        *out_moved = 0u;
        return true;
    }
    for (i = 0; i < num_tris; i++) {
        int32_t value = raw[i];
        int32_t fold;
        if (value == 0) {
            continue;            /* an excluded corner (e.g. TRI_FLAG_80) */
        }
        if (value == delta) {
            moved |= (uint16_t)(1u << i);
            continue;
        }
        fold = value - delta;
        if (fold % DKR_UV_SCROLL_FOLD_UNIT != 0) {
            return false;
        }
        moved |= (uint16_t)(1u << i);
    }
    *out_delta = delta;
    *out_moved = moved;
    return true;
}

/*
 * Read one batch's {T} bytes out of the retained task and diff them against the
 * live {T+1} bytes the next authored task is pointing at. Nothing here is
 * written back: the retained image stays immutable and the live array is only
 * read.
 */
static void dkr_capture_uv_scroll_endpoints(const Triangle *next,
                                            int num_tris) {
    const Triangle *prev;
    const void *retained;
    const void *previous_address = next;
    const void *key = next;
    size_t byte_size;
    int32_t raw_u[GFX_PRESENTATION_UV_SCROLL_MAX_TRIANGLES];
    int32_t raw_v[GFX_PRESENTATION_UV_SCROLL_MAX_TRIANGLES];
    GfxPresentationUvScroll scroll;
    int i;

    if (next == NULL || num_tris <= 0 ||
        num_tris > (int)GFX_PRESENTATION_UV_SCROLL_MAX_TRIANGLES) {
        return;
    }
    byte_size = (size_t)num_tris * sizeof(*next);
    {
        size_t room = dkr_arena_room(next);
        if (room == (size_t)-1 ? !dkr_ptr_plausible(next) : room < byte_size) {
            return;
        }
    }
    /* A game-declared ping-pong surface holds {T} in its OTHER buffer, and the
     * replay walk will hand us that other address. Both phases key off the
     * pair's even-phase address so the two ticks of one surface agree. */
    (void)dkr_paired_triangle_alias(next, &previous_address, &key);
    /* The key is an ORIGINAL address, which is what the replay walk translates
     * back to. A batch whose {T} bytes are not wholly retained has no endpoint
     * pair at all and simply holds. */
    retained = gfx_retained_task_retained_span(previous_address, byte_size);
    if (retained == NULL) {
        return;
    }
    prev = (const Triangle *)retained;
    for (i = 0; i < num_tris; i++) {
        /* Topology identity. The four index/flag bytes are the batch's shape;
         * if they moved, this address is not the same geometry it was on the
         * previous tick and no UV correspondence exists. */
        if (prev[i].vertices != next[i].vertices) {
            return;
        }
        /* All three corners of a triangle are shifted together by every
         * driver, so a disagreement between them is not a scroll. */
        raw_u[i] = (int32_t)next[i].uv0.u - (int32_t)prev[i].uv0.u;
        raw_v[i] = (int32_t)next[i].uv0.v - (int32_t)prev[i].uv0.v;
        if ((int32_t)next[i].uv1.u - (int32_t)prev[i].uv1.u != raw_u[i] ||
            (int32_t)next[i].uv2.u - (int32_t)prev[i].uv2.u != raw_u[i] ||
            (int32_t)next[i].uv1.v - (int32_t)prev[i].uv1.v != raw_v[i] ||
            (int32_t)next[i].uv2.v - (int32_t)prev[i].uv2.v != raw_v[i]) {
            return;
        }
    }
    memset(&scroll, 0, sizeof(scroll));
    scroll.triangle_count = (uint32_t)num_tris;
    if (!dkr_uv_scroll_resolve_axis(raw_u, num_tris, &scroll.du,
                                    &scroll.moved_u) ||
        !dkr_uv_scroll_resolve_axis(raw_v, num_tris, &scroll.dv,
                                    &scroll.moved_v)) {
        return;
    }
    if (scroll.du == 0 && scroll.dv == 0) {
        return;                  /* static geometry: the hold is already exact */
    }
    (void)gfx_presentation_packet_capture_uv_scroll(key, &scroll);
}

/*
 * Capture the already-authored next task's continuous vertex/effect streams.
 *
 * DKR double-buffers graphics tasks. Task T is submitted at the start of the
 * game pass, then task T+1 is completely built in the alternate buffer before
 * the host reaches its presentation subloop. Camera/object snapshots already
 * use that true forward state. This structural census gives deformation the
 * same {T,T+1} ownership without executing rendering, predicting simulation,
 * or adding a frame of latency.
 *
 * Only flow control, segment/billboard state, object-owner matrices, and G_VTX
 * batches are interpreted. No backend call, texture upload, primitive setup,
 * game callback, or authoritative write is reachable from this walker.
 */
static bool dkr_scan_future_deformations(Gfx *cmd, int depth, int limit) {
    Gfx *start;
    long safety = 0;

    if (cmd == NULL || depth >= DKR_DL_MAX_DEPTH) {
        return false;
    }
    start = cmd;
    for (;;) {
        if (limit > 0 && (cmd - start) >= limit) {
            return true;
        }
        if (++safety > 4000000L || dkr_arena_room(cmd) < sizeof(Gfx)) {
            return false;
        }
        switch ((uint8_t)C0(cmd, 24, 8)) {
            case G_DL: {
                uint8_t nopush = (uint8_t)C0(cmd, 16, 8);
                Gfx *sub = (Gfx *)dkr_resolve(cmd->words.w1);
                if (sub == NULL) {
                    if (nopush == G_DL_NOPUSH) {
                        return true;
                    }
                    break;
                }
                if (nopush == G_DL_NOPUSH) {
                    cmd = sub;
                    start = cmd;
                    continue;
                }
                if (!dkr_scan_future_deformations(sub, depth + 1, 0)) {
                    return false;
                }
                break;
            }
            case G_DMADL: {
                int count = (int)C0(cmd, 16, 8);
                Gfx *sub = (Gfx *)dkr_resolve(cmd->words.w1);
                if (count <= 0 || (sub != NULL &&
                    !dkr_scan_future_deformations(
                        sub, depth + 1, count))) {
                    return false;
                }
                break;
            }
            case (uint8_t)G_ENDDL:
                return true;
            case (uint8_t)G_MOVEWORD:
                /* The census needs only address resolution, billboard stream
                 * boundaries, and explicit matrix-slot selection. Running the
                 * full interpreter here would also touch fog/remaster-normal
                 * diagnostics, which are rendering side effects and do not
                 * belong in this read-only structural pass. */
                switch ((uint8_t)C0(cmd, 0, 8)) {
                    case G_MW_SEGMENT:
                    case G_MW_BILLBOARD:
                    case G_MW_MVPMATRIX:
                        dkr_sp_moveword(
                            (uint8_t)C0(cmd, 0, 8),
                            (uint16_t)C0(cmd, 8, 16), cmd->words.w1);
                        break;
                    default:
                        break;
                }
                break;
            case G_MTX: {
                uint8_t matrix_param = (uint8_t)C0(cmd, 16, 8);
                uint8_t draw_space = matrix_param & 7u;
                uint8_t effective_draw_space =
                    draw_space == G_MTX_DKR_SPACE_INHERIT
                        ? rsp.draw_space
                        : draw_space;
                bool world_matrix =
                    effective_draw_space == G_MTX_DKR_SPACE_WORLD;
                void *matrix = dkr_resolve(cmd->words.w1);
                GfxShadowMatrixBinding binding;
                bool stale = false;
                bool binding_valid;

                memset(&binding, 0, sizeof(binding));
                binding_valid = dkr_shadow_lookup_live(
                    matrix, &binding, &stale);
                if (world_matrix && binding_valid &&
                    binding.presentation_owner.valid &&
                    (binding.presentation_owner.matrix_class ==
                         GFX_PRESENTATION_MATRIX_ROOT ||
                     binding.presentation_owner.matrix_class ==
                         GFX_PRESENTATION_MATRIX_CHILD)) {
                    const GfxPresentationMatrixOwner *owner =
                        &binding.presentation_owner;
                    bool new_root = owner->matrix_class ==
                        GFX_PRESENTATION_MATRIX_ROOT;
                    bool new_lifetime =
                        !rsp.deformation_owner_valid ||
                        rsp.deformation_owner.address != owner->address ||
                        rsp.deformation_owner.generation != owner->generation ||
                        rsp.deformation_viewport != binding.viewport;
                    rsp.deformation_owner = *owner;
                    rsp.deformation_viewport = binding.viewport;
                    if (new_root || new_lifetime) {
                        rsp.deformation_owner_valid =
                            dkr_deformation_begin_stream(
                                owner, binding.viewport);
                    } else {
                        rsp.deformation_owner_valid = true;
                    }
                } else {
                    memset(&rsp.deformation_owner, 0,
                           sizeof(rsp.deformation_owner));
                    rsp.deformation_viewport = 0;
                    rsp.deformation_stream = 0u;
                    rsp.deformation_batch = 0u;
                    rsp.deformation_owner_valid = false;
                }
                if (world_matrix && binding_valid &&
                    binding.presentation_owner.valid &&
                    binding.presentation_owner.matrix_class ==
                        GFX_PRESENTATION_MATRIX_EFFECT) {
                    const GfxPresentationMatrixOwner *owner =
                        &binding.presentation_owner;
                    (void)gfx_presentation_packet_capture_deformation(
                        owner, binding.viewport, 0u, owner, sizeof(*owner), 1u,
                        (uint32_t)sizeof(*owner));
                }
                if (draw_space != G_MTX_DKR_SPACE_INHERIT) {
                    rsp.draw_space = draw_space;
                }
                break;
            }
            case G_VTX: {
                uint8_t parameter = (uint8_t)C0(cmd, 16, 8);
                int count = ((parameter >> 3) & 0x1f) + 1;
                const Vertex *vertices =
                    (const Vertex *)dkr_resolve(cmd->words.w1);
                GfxPresentationPacketBinding packet_binding;
                bool packet_vertex = false;

                if (vertices == NULL || count <= 0 || count > DKR_MAX_VERTICES ||
                    dkr_arena_room(vertices) <
                        (size_t)count * sizeof(*vertices)) {
                    break;
                }
                memset(&packet_binding, 0, sizeof(packet_binding));
                packet_vertex = gfx_presentation_packet_lookup_live_vertex(
                    vertices, &packet_binding);
                if (packet_vertex &&
                    packet_binding.owner.matrix_class ==
                        GFX_PRESENTATION_MATRIX_PARTICLE_VERTICES) {
                    (void)gfx_presentation_packet_capture_deformation(
                        &packet_binding.owner, packet_binding.viewport, 0u,
                        vertices, (size_t)count * sizeof(*vertices),
                        (uint32_t)count, (uint32_t)sizeof(*vertices));
                } else if (packet_vertex &&
                           packet_binding.owner.matrix_class ==
                               GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES &&
                           dkr_test_projected_shadow_vertex_lerp_enabled()) {
                    (void)gfx_presentation_packet_capture_deformation(
                        &packet_binding.owner, packet_binding.viewport,
                        packet_binding.ordinal, vertices,
                        (size_t)count * sizeof(*vertices), (uint32_t)count,
                        (uint32_t)sizeof(*vertices));
                } else if (!rsp.billboard &&
                           rsp.deformation_owner_valid) {
                    uint32_t ordinal;
                    if (dkr_deformation_next_ordinal(&ordinal)) {
                        (void)gfx_presentation_packet_capture_deformation(
                            &rsp.deformation_owner,
                            rsp.deformation_viewport, ordinal, vertices,
                            (size_t)count * sizeof(*vertices),
                            (uint32_t)count,
                            (uint32_t)sizeof(*vertices));
                    }
                }
                break;
            }
            case G_TRIN: {
                /* Authored UV scroll. Read-only, like the rest of the census:
                 * the batch's live {T+1} corner UVs are diffed against the
                 * retained {T} copy and only the resulting displacement is
                 * kept. No triangle bytes are stored and nothing is written. */
                uint8_t parameter = (uint8_t)C0(cmd, 16, 8);
                int num_tris = ((parameter >> 4) & 0xf) + 1;
                const Triangle *tris =
                    (const Triangle *)dkr_resolve(cmd->words.w1);
                if (tris != NULL) {
                    dkr_capture_uv_scroll_endpoints(tris, num_tris);
                }
                break;
            }
            default:
                break;
        }
        cmd++;
    }
}

bool gfx_dkr_capture_future_deformations(const Gfx *begin, const Gfx *end,
                                         uint64_t authored_tick) {
    uintptr_t saved_segments[16];
    GfxPresentationMatrixOwner saved_deformation_owner;
    struct DkrDeformationCursor
        saved_deformation_cursors[DKR_DEFORMATION_OWNERS];
    uintptr_t begin_address = (uintptr_t)begin;
    uintptr_t end_address = (uintptr_t)end;
    uint64_t target_tick = 0u;
    size_t command_bytes;
    size_t command_count;
    size_t saved_deformation_cursor_count;
    int saved_active_slot;
    int saved_deformation_viewport;
    uint32_t saved_deformation_stream;
    uint32_t saved_deformation_batch;
    uint8_t saved_draw_space;
    bool saved_world_safe_region;
    bool saved_billboard;
    bool saved_deformation_owner_valid;
    bool scanned;

    if (begin == NULL || end == NULL || end_address <= begin_address ||
        !present_sched_replay_armed() ||
        !presentation_snapshot_replay_target_tick(
            dkr_last_walked_authored_tick, &target_tick) ||
        target_tick != authored_tick) {
        return false;
    }
    if (dkr_future_last_published_tick == authored_tick) {
        return true;
    }
    command_bytes = (size_t)(end_address - begin_address);
    if (command_bytes % sizeof(*begin) != 0u) {
        gfx_presentation_packet_note_future_capture(false);
        return false;
    }
    command_count = command_bytes / sizeof(*begin);
    if (command_count == 0u || command_count > (size_t)INT_MAX) {
        gfx_presentation_packet_note_future_capture(false);
        return false;
    }
    memcpy(saved_segments, gfx_segment_table, sizeof(saved_segments));
    saved_active_slot = rsp.active_slot;
    saved_billboard = rsp.billboard;
    saved_draw_space = rsp.draw_space;
    saved_world_safe_region = rsp.world_safe_region;
    saved_deformation_owner = rsp.deformation_owner;
    saved_deformation_viewport = rsp.deformation_viewport;
    saved_deformation_stream = rsp.deformation_stream;
    saved_deformation_batch = rsp.deformation_batch;
    saved_deformation_owner_valid = rsp.deformation_owner_valid;
    saved_deformation_cursor_count = rsp.deformation_cursor_count;
    memcpy(saved_deformation_cursors, rsp.deformation_cursors,
           sizeof(saved_deformation_cursors));
    memset(rsp.deformation_cursors, 0, sizeof(rsp.deformation_cursors));
    rsp.deformation_cursor_count = 0u;
    memset(&rsp.deformation_owner, 0, sizeof(rsp.deformation_owner));
    rsp.deformation_owner_valid = false;
    rsp.deformation_viewport = 0;
    rsp.deformation_stream = 0u;
    rsp.deformation_batch = 0u;
    rsp.billboard = false;
    rsp.draw_space = G_MTX_DKR_SPACE_WORLD;
    rsp.world_safe_region = false;
    gfx_presentation_packet_capture_begin(authored_tick);
    scanned = dkr_scan_future_deformations(
        (Gfx *)begin, 0, (int)command_count);
    memcpy(gfx_segment_table, saved_segments, sizeof(saved_segments));
    rsp.active_slot = saved_active_slot;
    rsp.billboard = saved_billboard;
    rsp.draw_space = saved_draw_space;
    rsp.world_safe_region = saved_world_safe_region;
    rsp.deformation_owner = saved_deformation_owner;
    rsp.deformation_viewport = saved_deformation_viewport;
    rsp.deformation_stream = saved_deformation_stream;
    rsp.deformation_batch = saved_deformation_batch;
    rsp.deformation_owner_valid = saved_deformation_owner_valid;
    rsp.deformation_cursor_count = saved_deformation_cursor_count;
    memcpy(rsp.deformation_cursors, saved_deformation_cursors,
           sizeof(saved_deformation_cursors));
    if (!scanned) {
        gfx_presentation_packet_capture_abort();
        gfx_presentation_packet_publish_uv_scroll(0u);
        gfx_presentation_packet_note_future_capture(false);
        return false;
    }
    if (!gfx_presentation_packet_publish_deformation()) {
        gfx_presentation_packet_publish_uv_scroll(0u);
        gfx_presentation_packet_note_future_capture(false);
        return false;
    }
    gfx_presentation_packet_publish_uv_scroll(authored_tick);
    dkr_future_last_published_tick = authored_tick;
    gfx_presentation_packet_note_future_capture(true);
    return true;
}

typedef enum DkrRetainedVertexReplay {
    DKR_RETAINED_VERTEX_UNAVAILABLE = 0,
    DKR_RETAINED_VERTEX_HELD,
    DKR_RETAINED_VERTEX_INTERPOLATED,
} DkrRetainedVertexReplay;

typedef struct DkrEndpointVertexSemantic {
    uint64_t task_tick;
    uint64_t owner_address;
    uint64_t owner_generation;
    uint32_t viewport;
    uint32_t ordinal;
    uint32_t vertex_index;
    float position[3];
    uint8_t color[4];
} DkrEndpointVertexSemantic;

static void dkr_note_endpoint_vertex_semantic(
    const GfxPresentationMatrixOwner *owner, int viewport, uint32_t ordinal,
    uint32_t vertex_index, const Vertex *current, bool interpolated,
    const float position[3], const struct RGBA *color,
    bool color_interpolated) {
    DkrEndpointVertexSemantic expected;
    DkrEndpointVertexSemantic actual;

    if (!dkr_test_endpoint_vertex_bytes_enabled() ||
        dkr_replay_object_alpha_numerator != 0u || owner == NULL ||
        current == NULL) {
        return;
    }
    memset(&expected, 0, sizeof(expected));
    expected.task_tick = dkr_last_walked_authored_tick;
    expected.owner_address = (uint64_t)(uintptr_t)owner->address;
    expected.owner_generation = owner->generation;
    expected.viewport = (uint32_t)viewport;
    expected.ordinal = ordinal;
    expected.vertex_index = vertex_index;
    expected.position[0] = (float)current->x;
    expected.position[1] = (float)current->y;
    expected.position[2] = (float)current->z;
    expected.color[0] = current->r;
    expected.color[1] = current->g;
    expected.color[2] = current->b;
    expected.color[3] = current->a;

    actual = expected;
    if (interpolated && position != NULL) {
        memcpy(actual.position, position, sizeof(actual.position));
    }
    if (interpolated && color_interpolated && color != NULL) {
        actual.color[0] = color->r;
        actual.color[1] = color->g;
        actual.color[2] = color->b;
        actual.color[3] = color->a;
    }
    gfx_presentation_packet_note_endpoint_semantic(
        &expected, &actual, sizeof(expected));
}

static DkrRetainedVertexReplay dkr_replay_deformation_vertices(
    const GfxPresentationMatrixOwner *owner, int viewport, uint32_t ordinal,
    int count, bool particle, bool allow_interpolation,
    Vertex retained[DKR_MAX_VERTICES],
    float out[DKR_MAX_VERTICES][3],
    struct RGBA colors[DKR_MAX_VERTICES]) {
    GfxPresentationDeformationBinding deformation;
    uint64_t target_tick = 0u;
    bool compatible;
    bool phase_aligned;
    bool interpolate;
    bool changed = false;
    bool color_changed = false;
    const bool interpolate_color =
        dkr_replay_vertex_color_interpolation_enabled();

    if (owner == NULL || count <= 0 || count > DKR_MAX_VERTICES ||
        retained == NULL || out == NULL || colors == NULL) {
        return DKR_RETAINED_VERTEX_UNAVAILABLE;
    }
    compatible = particle
        ? presentation_snapshot_particle_deformation_compatible(
              owner->address, owner->generation)
        : presentation_snapshot_deformation_compatible(
              owner->address, owner->generation);
    phase_aligned = presentation_snapshot_replay_target_tick(
        dkr_last_walked_authored_tick, &target_tick);
    if (allow_interpolation && !compatible) {
        gfx_presentation_packet_note_deformation_incompatible();
    }
    interpolate = allow_interpolation && compatible && phase_aligned &&
        gfx_presentation_packet_lookup_deformation(
            owner, viewport, ordinal, target_tick,
            (uint32_t)count, (uint32_t)sizeof(*retained), &deformation);
    if (allow_interpolation && compatible && !interpolate) {
        gfx_presentation_packet_note_phase_hold(false);
    }
    if (!interpolate &&
        !gfx_presentation_packet_lookup_deformation_hold(
            owner, viewport, ordinal, dkr_last_walked_authored_tick,
            (uint32_t)count, (uint32_t)sizeof(*retained), &deformation)) {
        return DKR_RETAINED_VERTEX_UNAVAILABLE;
    }
    for (int index = 0; index < count; index++) {
        Vertex previous;
        Vertex current;
        memcpy(&previous,
               deformation.previous_bytes +
                   (size_t)index * deformation.stride,
               sizeof(previous));
        memcpy(&current,
               deformation.current_bytes +
                   (size_t)index * deformation.stride,
               sizeof(current));
        /* The replayed command stream belongs to the previous side of the
         * forward pair (task T). Keep all discrete Vertex fields -- UVs and
         * flags included -- from T until the next complete authored task is
         * presented. XYZ and shade RGBA receive explicit continuous overrides
         * below. This makes alpha zero byte-exact to the real task instead of
         * accidentally borrowing T+1 texture coordinates. */
        retained[index] = interpolate ? previous : current;
        if (!interpolate) {
            dkr_note_endpoint_vertex_semantic(
                owner, viewport, ordinal, (uint32_t)index, &current, false,
                NULL, NULL, false);
            continue;
        }
        if (previous.x != current.x || previous.y != current.y ||
            previous.z != current.z) {
            changed = true;
        }
        if (previous.r != current.r || previous.g != current.g ||
            previous.b != current.b || previous.a != current.a) {
            color_changed = true;
        }
        out[index][0] = presentation_lerp1(
            (float)previous.x, (float)current.x,
            dkr_replay_object_alpha_numerator,
            dkr_replay_object_alpha_denominator);
        out[index][1] = presentation_lerp1(
            (float)previous.y, (float)current.y,
            dkr_replay_object_alpha_numerator,
            dkr_replay_object_alpha_denominator);
        out[index][2] = presentation_lerp1(
            (float)previous.z, (float)current.z,
            dkr_replay_object_alpha_numerator,
            dkr_replay_object_alpha_denominator);
        if (interpolate_color) {
            colors[index].r = presentation_lerp_u8(
                previous.r, current.r, dkr_replay_object_alpha_numerator,
                dkr_replay_object_alpha_denominator);
            colors[index].g = presentation_lerp_u8(
                previous.g, current.g, dkr_replay_object_alpha_numerator,
                dkr_replay_object_alpha_denominator);
            colors[index].b = presentation_lerp_u8(
                previous.b, current.b, dkr_replay_object_alpha_numerator,
                dkr_replay_object_alpha_denominator);
            colors[index].a = presentation_lerp_u8(
                previous.a, current.a, dkr_replay_object_alpha_numerator,
                dkr_replay_object_alpha_denominator);
        }
        dkr_note_endpoint_vertex_semantic(
            owner, viewport, ordinal, (uint32_t)index, &previous, true,
            out[index], &colors[index], interpolate_color);
    }
    if (!interpolate) {
        return DKR_RETAINED_VERTEX_HELD;
    }
    if (interpolate_color) {
        gfx_presentation_packet_note_deformation_color(
            particle, color_changed);
        changed = changed || color_changed;
    }
    if (particle) {
        gfx_presentation_packet_note_particle_deformation(changed);
    } else if (owner->matrix_class ==
               GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES) {
        gfx_presentation_packet_note_projected_shadow_deformation(changed);
    }
    if (changed) {
        gfx_presentation_packet_note_deformation_override();
    }
    return DKR_RETAINED_VERTEX_INTERPOLATED;
}

/*
 * Coherent replay for terrain-projected decals. A projected mesh can
 * change receiver polygons between ticks even when its batch counts and fan
 * connectivity stay the same. Pairwise vertex lerp then morphs unrelated
 * ground samples, producing large midpoint shadow pulses. Translate the exact
 * authored mesh laterally by the owning
 * object's generation-safe snapshot delta instead: shape/area stay fixed and
 * the decal still follows the continuously presented racer. Y deliberately
 * remains receiver-authored: kart bounce/jump height must never lift its
 * ground decal. A semantic model/animation transition holds the whole batch.
 */
static DkrRetainedVertexReplay dkr_replay_projected_shadow_rigid(
    const GfxPresentationMatrixOwner *owner, const Vertex *authored, int count,
    Vertex retained[DKR_MAX_VERTICES],
    float out[DKR_MAX_VERTICES][3]) {
    PresentationObjectPose before;
    PresentationObjectPose target;
    float delta[3];
    bool changed = false;

    if (owner == NULL || authored == NULL || count <= 0 ||
        count > DKR_MAX_VERTICES || retained == NULL || out == NULL ||
        owner->matrix_class !=
            GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES ||
        !presentation_snapshot_deformation_compatible(
            owner->address, owner->generation) ||
        !presentation_snapshot_resolve_object_generation(
            owner->address, owner->generation, 0u,
            dkr_replay_object_alpha_denominator, &before) ||
        !presentation_snapshot_resolve_object_generation(
            owner->address, owner->generation,
            dkr_replay_object_alpha_numerator,
            dkr_replay_object_alpha_denominator, &target) ||
        !target.interpolated) {
        return DKR_RETAINED_VERTEX_UNAVAILABLE;
    }
    for (size_t axis = 0; axis < 3; axis++) {
        delta[axis] = target.position[axis] - before.position[axis];
        if (!isfinite(delta[axis])) {
            return DKR_RETAINED_VERTEX_UNAVAILABLE;
        }
    }
    delta[1] = 0.0f;
    changed = delta[0] != 0.0f || delta[2] != 0.0f;
    for (int index = 0; index < count; index++) {
        retained[index] = authored[index];
        out[index][0] = (float)authored[index].x + delta[0];
        out[index][1] = (float)authored[index].y + delta[1];
        out[index][2] = (float)authored[index].z + delta[2];
    }
    gfx_presentation_packet_note_projected_shadow_deformation(changed);
    if (changed) {
        gfx_presentation_packet_note_deformation_override();
    }
    return DKR_RETAINED_VERTEX_INTERPOLATED;
}

_Static_assert(sizeof(GfxPresentationMatrixOwner) <=
                   GFX_PRESENTATION_DEFORM_MAX_BYTES,
               "effect recipe must fit the retained history packet");

static bool dkr_replay_effect_world(
    const GfxPresentationMatrixOwner *owner, int viewport,
    float out[4][4]) {
    GfxPresentationDeformationBinding retained;
    GfxPresentationMatrixOwner previous;
    GfxPresentationMatrixOwner current;
    uint64_t target_tick;

    if (owner == NULL || out == NULL) {
        return false;
    }
    if (!presentation_snapshot_replay_target_tick(
            dkr_last_walked_authored_tick, &target_tick)) {
        gfx_presentation_packet_note_phase_hold(true);
        return false;
    }
    if (!gfx_presentation_packet_lookup_deformation(
            owner, viewport, 0u, target_tick, 1u,
            (uint32_t)sizeof(*owner), &retained) ||
        retained.byte_size != sizeof(*owner)) {
        gfx_presentation_packet_note_phase_hold(true);
        return false;
    }
    memcpy(&previous, retained.previous_bytes, sizeof(previous));
    memcpy(&current, retained.current_bytes, sizeof(current));
    if (!mdkr_camera_replay_effect_world(
            &previous, &current, dkr_replay_object_alpha_numerator,
            dkr_replay_object_alpha_denominator, out)) {
        return false;
    }
    gfx_presentation_packet_note_effect_override();
    return true;
}

/*
 * Resolve one triangle batch's presentation-only UV-scroll displacement at the
 * current replay alpha, or NULL to hold the authored phase.
 *
 * `tris` is the RETAINED copy the replay is drawing from, so the census's
 * identity — the original level-model address — has to be recovered before the
 * table can be consulted. Ownership stays keyed to that original address for
 * the same reason every other retained class does: the private image is where
 * the bytes live, not what they are.
 *
 * The displacement is the tick's whole advance, so alpha scales it directly:
 * phase(alpha) = phase(T) + alpha * (phase(T+1) - phase(T)). At alpha zero the
 * caller never reaches here and the authored bytes are emitted untouched; at
 * alpha one the next real endpoint has already replaced this task.
 */
static const float *dkr_replay_uv_scroll_offset(const Triangle *tris,
                                                int num_tris, float out[2],
                                                uint16_t *out_mask_u,
                                                uint16_t *out_mask_v) {
    GfxPresentationUvScroll scroll;
    const void *original;
    uint64_t target_tick;

    if (tris == NULL || out == NULL || num_tris <= 0 ||
        num_tris > (int)GFX_PRESENTATION_UV_SCROLL_MAX_TRIANGLES) {
        return NULL;
    }
    if (!presentation_snapshot_replay_target_tick(
            dkr_last_walked_authored_tick, &target_tick)) {
        return NULL;
    }
    original = gfx_retained_task_original_address(tris);
    if (original == NULL) {
        return NULL;
    }
    (void)dkr_paired_triangle_alias(original, NULL, &original);
    if (!gfx_presentation_packet_lookup_uv_scroll(
            original, target_tick, (uint32_t)num_tris, &scroll)) {
        return NULL;
    }
    out[0] = presentation_lerp1(
        0.0f, (float)scroll.du, dkr_replay_object_alpha_numerator,
        dkr_replay_object_alpha_denominator);
    out[1] = presentation_lerp1(
        0.0f, (float)scroll.dv, dkr_replay_object_alpha_numerator,
        dkr_replay_object_alpha_denominator);
    if (out[0] == 0.0f && out[1] == 0.0f) {
        /* Sub-unit alpha on a slow scroller: the authored phase already IS
         * this frame's nearest presentation phase. Not a fail-closed hold --
         * there is nothing to move. */
        return NULL;
    }
    *out_mask_u = scroll.moved_u;
    *out_mask_v = scroll.moved_v;
    gfx_presentation_packet_note_uv_scroll_override();
    return out;
}

static void dkr_run_dl(Gfx *cmd, int depth, int limit) {
    const bool census = dkr_dl_census_enabled();
    if (cmd == NULL) {
        dkr_dl_fault("null display-list pointer", cmd, depth);
        return;
    }
    if (depth >= DKR_DL_MAX_DEPTH) {
        dkr_dl_fault("display-list recursion limit exceeded", cmd, depth);
        return;
    }
    if (census) {
        s_dl_census_lists++;
        if ((unsigned)depth > s_dl_census_max_depth) {
            s_dl_census_max_depth = (unsigned)depth;
        }
    }
    Gfx *start = cmd;
    long safety = 0;

    for (;;) {
        if (limit > 0 && (cmd - start) >= limit) return;
        if (++safety > 4000000L) {
            dkr_dl_fault("unterminated display list", cmd, depth);
            return;
        }
        /* Never fetch a command past the arena: an arena-backed sub-list that
         * lacks a G_ENDDL terminator (or a mis-resolved DL pointer) would walk
         * off the 16 MB arena into an unmapped page. Non-arena DLs (rodata init
         * lists) return SIZE_MAX room and are trusted to self-terminate. */
        /* Stop if this list has walked off the top of the arena — a mis-decoded
         * or unterminated arena sub-list would otherwise fetch from the unmapped
         * page immediately after the 16 MB block. dkr_arena_room() returns
         * SIZE_MAX exactly AT arena_end (== is not < end), so guard the adjacent
         * band explicitly; genuine non-arena globals live far away and pass. */
        {
            uintptr_t uc = (uintptr_t)cmd;
            uintptr_t ab = (uintptr_t)g_dkrArenaBase;
            uintptr_t ae = ab + (uintptr_t)g_dkrArenaSize;
            if (uc >= ae && uc < ae + 0x00010000u) {
                dkr_dl_fault("display list walked beyond RDRAM", cmd, depth);
                return;
            }
        }
        if (dkr_arena_room(cmd) < sizeof(Gfx)) {
            dkr_dl_fault("truncated display-list command", cmd, depth);
            return;
        }
        uint8_t op = (uint8_t)C0(cmd, 24, 8);
        if (census) {
            s_dl_census_commands++;
            s_dl_census_opcodes[op]++;
        }
        switch (op) {

        /* ---- SP: flow control ---- */
        case G_SPNOOP:
            break;
        case G_DL: {  /* gSPDisplayList (push/call) / gSPBranchList (nopush) */
            uint8_t nopush = (uint8_t)C0(cmd, 16, 8);
            Gfx *sub = (Gfx *)dkr_resolve(cmd->words.w1);
            if (nopush == G_DL_NOPUSH) {
                if (sub == NULL) {
                    /*
                     * A conditional branch target may be left null by DKR's
                     * list builder. The pre-hardening interpreter treated
                     * that as an early end of the current list, and retail
                     * track-preview/menu streams rely on that behavior.
                     */
                    return;
                }
                cmd = sub;
                start = cmd;          /* rebase for any active limit */
                continue;             /* tail jump — do not advance */
            }
            /*
             * DKR also emits optional pushed lists with a null target. They
             * are an absent child, not evidence that the parent list is
             * corrupt; skipping them restores the original HLE semantics
             * while retaining strict faults for non-null invalid streams.
             */
            if (sub != NULL) {
                dkr_run_dl(sub, depth + 1, 0);
            }
            break;
        }
        case G_DMADL: {  /* gDkrDmaDisplayList — call, bounded by command count */
            int count = (int)C0(cmd, 16, 8);
            Gfx *sub = (Gfx *)dkr_resolve(cmd->words.w1);
            DTRACE("G_DMADL count=%d addr=%08x->%p depth=%d", count, cmd->words.w1,
                   (void *)sub, depth);
            if (count <= 0) {
                dkr_dl_fault("G_DMADL has a zero command count", cmd, depth);
            } else if (sub != NULL) {
                /* A null optional DMA child is absent, as for pushed G_DL. */
                dkr_run_dl(sub, depth + 1, count);
            }
            break;
        }
        case (uint8_t)G_ENDDL:
            return;

        /* ---- SP: geometry ---- */
        case G_MTX: {  /* gSPMatrixDKR — load slot and select it */
            uint8_t matrix_param = (uint8_t)C0(cmd, 16, 8);
            /* Loading a slot also SELECTS it (rsp.active_slot = slot below).
             * Source evidence, not inference: camera.c:2555 writes the sprite
             * matrix with gSPMatrixDKR(..., G_MTX_DKR_INDEX_2), enables
             * billboarding and draws — with no gSPSelectMatrixDKR in between —
             * and objects.c:6914 has to emit an explicit select back to
             * INDEX_1 after its INDEX_2 detour. Both only make sense if the
             * load selects. The same sweep confirms all 3,223,544 billboard
             * polygons were drawn with slot 2 active, as f3ddkr.h's
             * billboarding note requires. */
            /* Only G_MTX_DKR_INDEX_0..2 exist; rsp.mtx / rsp.shadow_matrix /
             * rsp.shadow_matrix_valid are all sized 3. Fold the unused fourth
             * encoding onto slot 0, as G_MW_MVPMATRIX does. Unreachable: the
             * same sweep counted G_MTX slot usage; slot 3 never occurred. */
            int slot = (int)((matrix_param >> 6) & 3);
            if (slot > 2) slot = 0;
            uint8_t draw_space = matrix_param & 7;
            uint8_t effective_draw_space =
                draw_space == G_MTX_DKR_SPACE_INHERIT
                    ? rsp.draw_space
                    : draw_space;
            bool world_matrix =
                effective_draw_space == G_MTX_DKR_SPACE_WORLD;
            void *ma = dkr_resolve(cmd->words.w1);
            const void *matrix_identity =
                gfx_retained_task_original_address(ma);
            float replay_world[4][4];
            bool object_overridden = false;
            bool effect_overridden = false;
            bool billboard_overridden = false;
            bool billboard_binding_found = false;
            bool shadow_matrix_stale = false;
            int32_t billboard_matrix[16];
            GfxPresentationPacketBinding packet_binding;
            rsp.active_slot = slot;
            memset(&rsp.shadow_matrix[slot], 0,
                   sizeof(rsp.shadow_matrix[slot]));
            rsp.shadow_matrix_valid[slot] =
                dkr_shadow_lookup_live(
                    ma, &rsp.shadow_matrix[slot], &shadow_matrix_stale);
            if (!dkr_replay_pass && ma != NULL &&
                dkr_arena_room(ma) >= sizeof(Mtx)) {
                (void)gfx_retained_task_capture_dependency(
                    ma, ma, sizeof(Mtx));
                (void)gfx_shadow_matrix_note_walked_key(
                    ma, ma, sizeof(Mtx));
                (void)gfx_presentation_packet_note_walked_matrix(
                    ma, ma, sizeof(Mtx));
            }
            /* A ROOT begins one model's deterministic G_VTX sequence. CHILD
             * matrices keep that sequence alive for articulated parts. Any
             * other world matrix closes it, preventing ownership from leaking
             * into unrelated geometry later in the display list. */
            if (world_matrix && rsp.shadow_matrix_valid[slot] &&
                rsp.shadow_matrix[slot].presentation_owner.valid &&
                (rsp.shadow_matrix[slot].presentation_owner.matrix_class ==
                     GFX_PRESENTATION_MATRIX_ROOT ||
                 rsp.shadow_matrix[slot].presentation_owner.matrix_class ==
                     GFX_PRESENTATION_MATRIX_CHILD)) {
                const GfxPresentationMatrixOwner *owner =
                    &rsp.shadow_matrix[slot].presentation_owner;
                bool new_root =
                    owner->matrix_class == GFX_PRESENTATION_MATRIX_ROOT;
                bool new_lifetime =
                    !rsp.deformation_owner_valid ||
                    rsp.deformation_owner.address != owner->address ||
                    rsp.deformation_owner.generation != owner->generation ||
                    rsp.deformation_viewport !=
                        rsp.shadow_matrix[slot].viewport;
                rsp.deformation_owner = *owner;
                rsp.deformation_viewport = rsp.shadow_matrix[slot].viewport;
                if (new_root || new_lifetime) {
                    rsp.deformation_owner_valid =
                        dkr_deformation_begin_stream(
                            owner, rsp.shadow_matrix[slot].viewport);
                } else {
                    rsp.deformation_owner_valid = true;
                }
            } else {
                memset(&rsp.deformation_owner, 0,
                       sizeof(rsp.deformation_owner));
                rsp.deformation_viewport = 0;
                rsp.deformation_stream = 0u;
                rsp.deformation_batch = 0u;
                rsp.deformation_owner_valid = false;
            }
            if (world_matrix && !dkr_replay_pass &&
                rsp.shadow_matrix_valid[slot] &&
                rsp.shadow_matrix[slot].presentation_owner.valid &&
                rsp.shadow_matrix[slot].presentation_owner.matrix_class ==
                    GFX_PRESENTATION_MATRIX_EFFECT) {
                const GfxPresentationMatrixOwner *effect_owner =
                    &rsp.shadow_matrix[slot].presentation_owner;
                (void)gfx_presentation_packet_capture_deformation(
                    effect_owner, rsp.shadow_matrix[slot].viewport, 0u,
                    effect_owner, sizeof(*effect_owner), 1u,
                    (uint32_t)sizeof(*effect_owner));
            }
            if (world_matrix && dkr_replay_pass &&
                rsp.shadow_matrix_valid[slot] &&
                dkr_replay_object_alpha_valid &&
                dkr_replay_object_alpha_numerator != 0u) {
                const GfxPresentationMatrixOwner *presentation_owner =
                    &rsp.shadow_matrix[slot].presentation_owner;
                const bool effect_owner =
                    presentation_owner->valid &&
                    presentation_owner->matrix_class ==
                        GFX_PRESENTATION_MATRIX_EFFECT;
                if (effect_owner) {
                    if (dkr_replay_effect_interpolation_enabled()) {
                        effect_overridden = dkr_replay_effect_world(
                            presentation_owner,
                            rsp.shadow_matrix[slot].viewport, replay_world);
                    }
                } else {
                    object_overridden = mdkr_camera_replay_object_world(
                        presentation_owner,
                        dkr_replay_object_alpha_numerator,
                        dkr_replay_object_alpha_denominator, replay_world);
                }
                if (presentation_owner->valid && !effect_owner) {
                    if (object_overridden) {
                        dkr_replay_object_hits++;
                    } else {
                        dkr_replay_object_holds++;
                    }
                }
            }
            if (world_matrix && dkr_replay_pass &&
                dkr_replay_object_alpha_valid) {
                billboard_binding_found =
                    gfx_presentation_packet_lookup_matrix_observed(
                        matrix_identity, ma, &packet_binding);
            }
            if (billboard_binding_found) {
                if (dkr_replay_object_alpha_numerator != 0u) {
                    billboard_overridden = mdkr_camera_replay_billboard_matrix(
                        &packet_binding.owner, packet_binding.viewport,
                        dkr_replay_object_alpha_numerator,
                        dkr_replay_object_alpha_denominator,
                        billboard_matrix);
                }
                if (billboard_overridden) {
                    dkr_replay_billboard_matrix_hits++;
                    if (packet_binding.stale) {
                        gfx_presentation_packet_note_stale_hold(true, true);
                    }
                } else {
                    dkr_replay_billboard_matrix_holds++;
                }
            }
            if (dkr_replay_pass && dkr_replay_object_alpha_valid) {
                const GfxPresentationMatrixOwner *opacity_owner = NULL;
                if (world_matrix && billboard_binding_found) {
                    opacity_owner = &packet_binding.owner;
                } else if (world_matrix && rsp.shadow_matrix_valid[slot]) {
                    opacity_owner =
                        &rsp.shadow_matrix[slot].presentation_owner;
                }
                dkr_replay_bind_opacity_owner(opacity_owner);
            }
            if (billboard_overridden) {
                if (packet_binding.stale &&
                    dkr_replay_object_alpha_numerator == 0u) {
                    gfx_presentation_packet_note_dependency_endpoint(
                        packet_binding.key_bytes, billboard_matrix,
                        sizeof(billboard_matrix));
                }
                dkr_decode_matrix(slot, billboard_matrix);
            } else if (dkr_replay_pass && billboard_binding_found &&
                       packet_binding.stale) {
                if (packet_binding.key_size == sizeof(Mtx)) {
                    int32_t retained_matrix[16];
                    memcpy(retained_matrix, packet_binding.key_bytes,
                           sizeof(retained_matrix));
                    if (dkr_replay_object_alpha_numerator == 0u) {
                        gfx_presentation_packet_note_dependency_endpoint(
                            packet_binding.key_bytes,
                            packet_binding.key_bytes,
                            packet_binding.key_size);
                    }
                    dkr_decode_matrix(slot, retained_matrix);
                    gfx_presentation_packet_note_stale_hold(true, true);
                } else {
                    gfx_presentation_packet_note_stale_hold(true, false);
                    dkr_replay_dependency_failed = true;
                }
            } else if (world_matrix && dkr_replay_pass &&
                rsp.shadow_matrix_valid[slot] &&
                (rsp.shadow_matrix[slot].vp_overridden ||
                 object_overridden ||
                 effect_overridden ||
                 (dkr_replay_force_recompose &&
                  rsp.shadow_matrix[slot].gameplay_vp))) {
                /*
                 * SELF-VALIDATING RECOMPOSITION.
                 *
                 * To draw this matrix under a different camera the replay must
                 * rebuild it from the registry's (world, view_projection)
                 * decomposition, through the game's own mtxf_mul/mtxf_to_mtx
                 * (mdkr_camera_replay_mvp). That is only sound if the
                 * decomposition actually reproduces the matrix the game built —
                 * and it does not always. mtx_head_rotation (camera.c) computes
                 * its list matrix as head x (parentWorld x VP) but must
                 * register a world of (head x parentWorld). Over a 3600-tick
                 * route, blindly recomposing every gameplay matrix moved 5178
                 * of 35119 of them, the worst by 152,081,586 s15.16 LSBs —
                 * about 2320 world units. That is not rounding, and shipping it
                 * would have teleported geometry on every interpolated frame.
                 *
                 * So the replay PROVES the decomposition per matrix, per frame:
                 * recompose with the view-projection AS CAPTURED and compare
                 * against the display list's own bytes. Bit-identical means the
                 * decomposition is faithful and the same arithmetic with a
                 * different camera is trustworthy. Anything else keeps the
                 * list's matrix — that object holds its authoritative pose for
                 * this frame, which is exactly what camera-only interpolation
                 * already does for every object, and is never worse than not
                 * replaying at all.
                 *
                 * GEOMETRIC TOLERANCE (only when the camera was overridden).
                 * Bit-exact equality is the wrong test for a decomposition whose
                 * only sin is re-associating a float product: 99.96% of the
                 * mismatches measured across all 63 levels are ≤ a few thousand
                 * s15.16 LSBs — thousandths to hundredths of a world unit — and
                 * are correct associations that rounded differently. Rejecting
                 * them keeps the TICK's camera for that geometry, so an
                 * interpolated frame mixed two cameras across up to a fifth of
                 * its scene. On the vp-overridden path the verification is
                 * therefore a distance, not an identity:
                 * DKR_RECOMPOSE_TOLERANCE_LSB above.
                 *
                 * The NON-overridden path stays BIT-EXACT, deliberately. There
                 * the display list already holds the exact matrix, so a
                 * tolerance could only permit a needless precision loss — and
                 * check_render_purity.py's arm E forces exactly that path and
                 * asserts the replay overdraws IDENTICALLY. Widening it here
                 * would disarm that arm.
                 *
                 * Nothing here writes to the captured display list or to game
                 * memory: both matrices are built on the stack (spec 4.5).
                 */
                int32_t verify[16];
                bool faithful = false;
                bool tolerated = false;
                int64_t worst = -1;
                if (ma != NULL && dkr_arena_room(ma) >= sizeof(Mtx)) {
                    mdkr_camera_replay_mvp(
                        rsp.shadow_matrix[slot].world,
                        rsp.shadow_matrix[slot].captured_view_projection,
                        verify);
                    if (dkr_test_recompose_reject_enabled() &&
                        !dkr_test_recompose_reject_used) {
                        dkr_mtx_inject_recompose_reject(ma, verify);
                        dkr_test_recompose_reject_used = true;
                    }
                    faithful = memcmp(verify, ma, sizeof(verify)) == 0;
                    if (!faithful) {
                        worst = dkr_mtx_worst_lsb_delta(ma, verify);
                        if ((rsp.shadow_matrix[slot].vp_overridden ||
                             object_overridden) &&
                            worst <= (int64_t)DKR_RECOMPOSE_TOLERANCE_LSB) {
                            faithful = true;
                            tolerated = true;
                        }
                    }
                }
                /*
                 * The census bins EVERY mismatch, accepted or not. Binning only
                 * the hard rejects would make the histogram change shape the
                 * moment the threshold moves, which is precisely the property a
                 * measurement used to CHOOSE the threshold must not have.
                 * `worst < 0` is an unusable matrix pointer: a reject with no
                 * geometry to measure, so it is counted as a reject and not as
                 * a magnitude.
                 */
                if (worst >= 0) {
                    present_perf_note_matrix_reject((uint64_t)worst);
                }
                if (tolerated) {
                    dkr_replay_matrix_tolerant++;
                    if ((uint64_t)worst > dkr_replay_matrix_tolerant_worst) {
                        dkr_replay_matrix_tolerant_worst = (uint64_t)worst;
                    }
                }
                if (faithful) {
                    int32_t recomposed[16];
                    mdkr_camera_replay_mvp(
                        (object_overridden || effect_overridden)
                            ? replay_world
                            : rsp.shadow_matrix[slot].world,
                        rsp.shadow_matrix[slot].view_projection,
                        recomposed);
                    dkr_decode_matrix(slot, recomposed);
                    dkr_replay_matrix_hits++;
                } else {
                    dkr_replay_matrix_rejects++;
                    /*
                     * HARD-REJECT IDENTIFICATION (MDKR_PRESENT_PERF). Once the
                     * tolerance has absorbed the re-association population, what
                     * is left is small enough to name individually instead of
                     * counting — and a reject that is only counted can never be
                     * root-caused. Prints the display-list matrix pointer, the
                     * registration context, and the translation row of both the
                     * registered world and the list's own matrix, which is what
                     * identifies WHICH object moved and by how far.
                     */
                    if (present_perf_enabled() && worst >= 0) {
                        const uint32_t *listed = (const uint32_t *)ma;
                        fprintf(stderr,
                                "[PRESENTREJECT-ID] frame=%d slot=%d key=%p "
                                "worstlsb=%lld site=%d mobility=%d viewport=%d "
                                "gameplayvp=%d vpoverridden=%d "
                                "world3=[%.3f %.3f %.3f] "
                                "listw3=%08x listw11=%08x\n",
                                dkr_frame_index, slot, ma, (long long)worst,
                                rsp.shadow_matrix[slot].site,
                                (int)rsp.shadow_matrix[slot].mobility,
                                rsp.shadow_matrix[slot].viewport,
                                (int)rsp.shadow_matrix[slot].gameplay_vp,
                                (int)rsp.shadow_matrix[slot].vp_overridden,
                                rsp.shadow_matrix[slot].world[3][0],
                                rsp.shadow_matrix[slot].world[3][1],
                                rsp.shadow_matrix[slot].world[3][2],
                                listed[3], listed[11]);
                    }
                    /*
                     * REJECT MAGNITUDE. `worst` is -1 only when the matrix
                     * pointer itself was unusable (null or off the arena), which
                     * is a reject for a reason that has no geometry to measure.
                     *
                     * The LEAST rejected magnitude is the interesting statistic,
                     * not the worst: it is the distance from the tolerance to
                     * the nearest thing the tolerance did NOT swallow, i.e. the
                     * live separation margin. The gates assert on it so the
                     * margin is re-proved every run instead of being inherited
                     * from this file's comment.
                     */
                    if (worst >= 0 &&
                        (!dkr_replay_matrix_reject_least_set ||
                         (uint64_t)worst < dkr_replay_matrix_reject_least)) {
                        dkr_replay_matrix_reject_least = (uint64_t)worst;
                        dkr_replay_matrix_reject_least_set = true;
                    }
                    dkr_load_matrix(slot, ma);
                }
            } else {
                if (dkr_replay_pass) {
                    dkr_replay_matrix_misses++;
                }
                if (dkr_replay_pass && shadow_matrix_stale &&
                    rsp.shadow_matrix[slot].walked_key_bytes_valid) {
                    dkr_decode_matrix(
                        slot, rsp.shadow_matrix[slot].walked_key_bytes);
                    gfx_presentation_packet_note_stale_hold(true, true);
                } else {
                    if (dkr_replay_pass && shadow_matrix_stale) {
                        gfx_presentation_packet_note_stale_hold(true, false);
                        dkr_replay_dependency_failed = true;
                    } else {
                        dkr_load_matrix(slot, ma);
                    }
                }
            }
            if (draw_space != G_MTX_DKR_SPACE_INHERIT) {
                dkr_set_draw_space(draw_space);
            }
            if (dkr_trace_this_frame && ma && dkr_arena_room(ma) >= sizeof(Mtx)) {
                const uint32_t *w = (const uint32_t *)ma;
                DTRACE("  mtx raw w[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x",
                       w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
            DTRACE("G_MTX slot=%d space=%u addr=%08x->%p m0=[%.3f %.3f %.3f %.3f] "
                   "m3=[%.3f %.3f %.3f %.3f]",
                   slot, draw_space, cmd->words.w1, ma,
                   rsp.mtx[slot][0][0], rsp.mtx[slot][0][1], rsp.mtx[slot][0][2], rsp.mtx[slot][0][3],
                   rsp.mtx[slot][3][0], rsp.mtx[slot][3][1], rsp.mtx[slot][3][2], rsp.mtx[slot][3][3]);
            break;
        }
        case G_VTX: {  /* gSPVertexDKR */
            uint8_t p = (uint8_t)C0(cmd, 16, 8);
            int n = ((p >> 3) & 0x1f) + 1;
            bool append = (p & 1) != 0;
            const Vertex *v = (const Vertex *)dkr_resolve(cmd->words.w1);
            const void *vertex_identity =
                gfx_retained_task_original_address(v);
            const Vertex *vertex_source = v;
            int retained_n = n;
            bool retained_packet_vertex = false;
            bool packet_binding_found = false;
            DkrRetainedVertexReplay retained_replay =
                DKR_RETAINED_VERTEX_UNAVAILABLE;
            Vertex retained_vertices[DKR_MAX_VERTICES];
            float position_overrides[DKR_MAX_VERTICES][3];
            const float (*position_override)[3] = NULL;
            struct RGBA color_overrides[DKR_MAX_VERTICES];
            const struct RGBA *color_override = NULL;
            GfxPresentationPacketBinding packet_binding;
            if (v != NULL) {
                const size_t room = dkr_arena_room(v);
                if (room != (size_t)-1 &&
                    (size_t)retained_n * sizeof(*v) > room) {
                    retained_n = (int)(room / sizeof(*v));
                } else if (room == (size_t)-1 && !dkr_ptr_plausible(v)) {
                    retained_n = 0;
                }
            }
            memset(&packet_binding, 0, sizeof(packet_binding));
            retained_packet_vertex = dkr_replay_pass
                ? gfx_presentation_packet_has_frozen_vertex(vertex_identity)
                : gfx_presentation_packet_has_live_vertex(v);
            if (!dkr_replay_pass && retained_packet_vertex && v != NULL &&
                retained_n > 0) {
                (void)gfx_presentation_packet_note_walked_vertex(
                    v, v, (size_t)retained_n * sizeof(*v));
            }
            if (v != NULL) {
                if (dkr_replay_pass && dkr_replay_object_alpha_valid) {
                    packet_binding_found =
                        gfx_presentation_packet_lookup_vertex_observed(
                            vertex_identity, v, &packet_binding);
                } else if (!dkr_replay_pass) {
                    packet_binding_found =
                        gfx_presentation_packet_lookup_live_vertex(
                            v, &packet_binding);
                }
            }
            if (!dkr_replay_pass && v != NULL && retained_n > 0) {
                (void)gfx_retained_task_capture_dependency(
                    v, v, (size_t)retained_n * sizeof(*v));
                /* Remastered per-pixel lighting consumes a parallel compact
                 * normal stream selected by the preceding MOVEWORD. It is a
                 * render dependency just like the vertex batch and may live
                 * outside the reusable arena, so retain the exact span the
                 * real walk is about to consume. */
                if (rsp.smooth_normals != NULL) {
                    const size_t normal_bytes =
                        (size_t)retained_n * sizeof(*rsp.smooth_normals);
                    const size_t normal_room =
                        dkr_arena_room(rsp.smooth_normals);
                    if ((normal_room == (size_t)-1 &&
                         dkr_ptr_plausible(rsp.smooth_normals)) ||
                        normal_room >= normal_bytes) {
                        (void)gfx_retained_task_capture_dependency(
                            rsp.smooth_normals, rsp.smooth_normals,
                            normal_bytes);
                    }
                }
            }
            if (dkr_replay_pass && packet_binding_found &&
                packet_binding.stale) {
                if (packet_binding.key_size >= sizeof(*v) &&
                    packet_binding.key_size % sizeof(*v) == 0u) {
                    retained_n = (int)(packet_binding.key_size / sizeof(*v));
                    if (retained_n > n) {
                        retained_n = n;
                    }
                    memcpy(retained_vertices, packet_binding.key_bytes,
                           (size_t)retained_n * sizeof(*v));
                    vertex_source = retained_vertices;
                    gfx_presentation_packet_note_stale_hold(false, true);
                } else {
                    gfx_presentation_packet_note_stale_hold(false, false);
                    dkr_replay_dependency_failed = true;
                    retained_n = 0;
                }
            }
            if (dkr_replay_pass && dkr_replay_object_alpha_valid &&
                packet_binding_found &&
                packet_binding.owner.matrix_class ==
                    GFX_PRESENTATION_MATRIX_BILLBOARD &&
                dkr_replay_object_alpha_numerator != 0u) {
                if (mdkr_camera_replay_billboard_anchor(
                        &packet_binding.owner,
                        dkr_replay_object_alpha_numerator,
                        dkr_replay_object_alpha_denominator,
                        position_overrides[0])) {
                    /* Only the anchor is interpolated; dkr_sp_vertex reads an
                     * override for EVERY vertex in the batch, so the rest must
                     * carry their own authored positions -- from the source
                     * this replay actually draws. When the stale-binding
                     * branch selected retained_vertices, `v` is the reused
                     * live memory that made the binding stale in the first
                     * place. */
                    for (int bi = 1; vertex_source != NULL && bi < retained_n;
                         bi++) {
                        position_overrides[bi][0] = (float)vertex_source[bi].x;
                        position_overrides[bi][1] = (float)vertex_source[bi].y;
                        position_overrides[bi][2] = (float)vertex_source[bi].z;
                    }
                    position_override = position_overrides;
                    dkr_replay_billboard_vertex_hits++;
                } else {
                    dkr_replay_billboard_vertex_holds++;
                }
            }
            if (packet_binding_found && retained_n > 0 &&
                packet_binding.owner.matrix_class ==
                    GFX_PRESENTATION_MATRIX_PARTICLE_VERTICES) {
                if (dkr_replay_pass && dkr_replay_object_alpha_valid) {
                    dkr_replay_bind_opacity_owner(&packet_binding.owner);
                }
                const size_t byte_size =
                    (size_t)retained_n * sizeof(*v);
                if (!dkr_replay_pass) {
                    (void)gfx_presentation_packet_capture_deformation(
                        &packet_binding.owner, packet_binding.viewport, 0u,
                        v, byte_size, (uint32_t)retained_n,
                        (uint32_t)sizeof(*v));
                } else {
                    retained_replay = dkr_replay_deformation_vertices(
                        &packet_binding.owner, packet_binding.viewport, 0u,
                        retained_n, true,
                        dkr_replay_object_alpha_valid &&
                            dkr_replay_particle_interpolation_enabled(),
                        retained_vertices, position_overrides,
                        color_overrides);
                    if (retained_replay !=
                            DKR_RETAINED_VERTEX_UNAVAILABLE) {
                        vertex_source = retained_vertices;
                    }
                    if (retained_replay ==
                            DKR_RETAINED_VERTEX_INTERPOLATED) {
                        position_override = position_overrides;
                    }
                    if (retained_replay ==
                            DKR_RETAINED_VERTEX_INTERPOLATED &&
                        dkr_replay_vertex_color_interpolation_enabled()) {
                        color_override = color_overrides;
                    }
                }
            }
            if (packet_binding_found &&
                retained_n > 0 &&
                packet_binding.owner.matrix_class ==
                    GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES) {
                /* This batch is a direct world-space recipe rather than an
                 * ordinary matrix-owned mesh. It still owns subsequent
                 * primitive-alpha commands: without rebinding here, a fading
                 * racer/Taj carpet decal could inherit a prior object's
                 * opacity during a retained replay. */
                if (dkr_replay_pass && dkr_replay_object_alpha_valid) {
                    dkr_replay_bind_opacity_owner(&packet_binding.owner);
                }
                const size_t byte_size =
                    (size_t)retained_n * sizeof(*v);
                bool rigid_translation = false;
                if (!dkr_replay_pass) {
                    if (dkr_test_projected_shadow_vertex_lerp_enabled()) {
                        (void)gfx_presentation_packet_capture_deformation(
                            &packet_binding.owner, packet_binding.viewport,
                            packet_binding.ordinal, v, byte_size,
                            (uint32_t)retained_n, (uint32_t)sizeof(*v));
                    }
                } else {
                    rigid_translation =
                        dkr_replay_object_alpha_valid &&
                        dkr_replay_object_alpha_numerator != 0u &&
                        dkr_replay_deformation_interpolation_enabled() &&
                        dkr_replay_projected_shadow_interpolation_enabled() &&
                        !dkr_test_projected_shadow_vertex_lerp_enabled();
                    if (rigid_translation) {
                        retained_replay =
                            dkr_replay_projected_shadow_rigid(
                                &packet_binding.owner, v, retained_n,
                                retained_vertices, position_overrides);
                    } else if (dkr_test_projected_shadow_vertex_lerp_enabled()) {
                        retained_replay = dkr_replay_deformation_vertices(
                            &packet_binding.owner, packet_binding.viewport,
                            packet_binding.ordinal, retained_n, false,
                            dkr_replay_object_alpha_valid &&
                                dkr_replay_deformation_interpolation_enabled() &&
                                dkr_replay_projected_shadow_interpolation_enabled(),
                            retained_vertices, position_overrides,
                            color_overrides);
                    }
                    if (retained_replay !=
                            DKR_RETAINED_VERTEX_UNAVAILABLE) {
                        vertex_source = retained_vertices;
                    }
                    if (retained_replay ==
                            DKR_RETAINED_VERTEX_INTERPOLATED) {
                        position_override = position_overrides;
                    }
                    if (retained_replay ==
                            DKR_RETAINED_VERTEX_INTERPOLATED &&
                        !rigid_translation &&
                        dkr_replay_vertex_color_interpolation_enabled()) {
                        color_override = color_overrides;
                    }
                }
            }
            /* Sprite anchors have their own retained position recipe above.
             * For ordinary model batches, retain the exact authored vertices
             * on the real walk and blend XYZ plus shade RGBA during replay.
             * Model, animation, generation, tick adjacency, batch count and
             * stride all have to agree; every refusal leaves the current
             * bytes in place. Texture coordinates remain authored-discrete. */
            if (v != NULL && retained_n > 0 && !rsp.billboard &&
                rsp.deformation_owner_valid) {
                uint32_t ordinal = 0u;
                const size_t byte_size = (size_t)retained_n * sizeof(*v);
                if (!dkr_deformation_next_ordinal(&ordinal)) {
                    rsp.deformation_owner_valid = false;
                } else if (!dkr_replay_pass &&
                           !retained_packet_vertex) {
                    (void)gfx_presentation_packet_capture_deformation(
                        &rsp.deformation_owner, rsp.deformation_viewport,
                        ordinal, v, byte_size, (uint32_t)retained_n,
                        (uint32_t)sizeof(*v));
                } else if (dkr_replay_pass &&
                           !retained_packet_vertex) {
                    retained_replay = dkr_replay_deformation_vertices(
                        &rsp.deformation_owner,
                        rsp.deformation_viewport, ordinal, retained_n, false,
                        dkr_replay_object_alpha_valid &&
                            dkr_replay_deformation_interpolation_enabled(),
                        retained_vertices, position_overrides,
                        color_overrides);
                    if (retained_replay !=
                            DKR_RETAINED_VERTEX_UNAVAILABLE) {
                        vertex_source = retained_vertices;
                    }
                    if (retained_replay ==
                            DKR_RETAINED_VERTEX_INTERPOLATED) {
                        position_override = position_overrides;
                    }
                    if (retained_replay ==
                            DKR_RETAINED_VERTEX_INTERPOLATED &&
                        dkr_replay_vertex_color_interpolation_enabled()) {
                        color_override = color_overrides;
                    }
                }
            }
            DTRACE("G_VTX n=%d append=%d slot=%d bb=%d addr=%08x->%p", n, append,
                   rsp.active_slot, rsp.billboard, cmd->words.w1, (const void *)v);
            if (dkr_replay_pass && packet_binding_found &&
                packet_binding.stale && retained_n > 0 &&
                packet_binding.key_size >= sizeof(Vertex)) {
                Vertex expected;
                memcpy(&expected, packet_binding.key_bytes, sizeof(expected));
                dkr_note_endpoint_vertex_semantic(
                    &packet_binding.owner, packet_binding.viewport, 0u, 0u,
                    &expected, position_override != NULL,
                    position_override != NULL ? position_override[0] : NULL,
                    color_override != NULL ? &color_override[0] : NULL,
                    color_override != NULL);
            }
            if (v) {
                /* retained_n is the count the retained/override arrays were
                 * actually filled to; n is the display list's request. Passing n
                 * would transform whatever the stack held past that point. */
                dkr_sp_vertex(
                    vertex_source, retained_n, append, position_override,
                    color_override);
            }
            break;
        }
        case G_TRIN: {  /* gSPPolygon */
            uint8_t p = (uint8_t)C0(cmd, 16, 8);
            int num_tris = ((p >> 4) & 0xf) + 1;
            bool tex = (p & 0x0f & 1) != 0;
            const Triangle *t = (const Triangle *)dkr_resolve(cmd->words.w1);
            DTRACE("G_TRIN ntris=%d tex=%d addr=%08x->%p", num_tris, tex, cmd->words.w1, (const void *)t);
            if (t) {
                float uv_offset[2];
                const float *uv_offset_ptr = NULL;
                uint16_t uv_mask_u = 0u;
                uint16_t uv_mask_v = 0u;

                if (!dkr_replay_pass) {
                    (void)gfx_retained_task_capture_dependency(
                        t, t, (size_t)num_tris * sizeof(*t));
                } else if (dkr_replay_object_alpha_valid &&
                           dkr_replay_object_alpha_numerator != 0u &&
                           dkr_replay_uv_scroll_interpolation_enabled()) {
                    uv_offset_ptr = dkr_replay_uv_scroll_offset(
                        t, num_tris, uv_offset, &uv_mask_u, &uv_mask_v);
                }
                dkr_begin_primitive(
                    rsp.draw_space != G_MTX_DKR_SPACE_WORLD);
                dkr_sp_polygon(t, num_tris, tex, uv_offset_ptr, uv_mask_u,
                               uv_mask_v);
            }
            break;
        }
        case (uint8_t)G_MOVEWORD:
            DTRACE("G_MOVEWORD index=%02x offset=%u data=%08x",
                   (uint8_t)C0(cmd, 0, 8), (uint16_t)C0(cmd, 8, 16), cmd->words.w1);
            dkr_sp_moveword((uint8_t)C0(cmd, 0, 8), (uint16_t)C0(cmd, 8, 16), cmd->words.w1);
            break;
        case G_MOVEMEM: {  /* gSPViewport (G_MV_VIEWPORT) */
            uint8_t idx = (uint8_t)C0(cmd, 16, 8);
            const void *data = dkr_resolve(cmd->words.w1);
            if (idx == G_MV_VIEWPORT && data) {
                if (!dkr_replay_pass) {
                    (void)gfx_retained_task_capture_dependency(
                        data, data, sizeof(Vp_t));
                }
                const Vp_t *vp = (const Vp_t *)data;
                dkr_calc_viewport(vp);
                DTRACE("G_MOVEMEM VIEWPORT scale=[%d %d %d] trans=[%d %d %d] -> vp{x=%d y=%d w=%d h=%d}",
                       vp->vscale[0], vp->vscale[1], vp->vscale[2],
                       vp->vtrans[0], vp->vtrans[1], vp->vtrans[2],
                       rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            } else {
                dkr_dl_fault("unsupported or unresolved G_MOVEMEM", cmd,
                             depth);
            }
            break;
        }
        case (uint8_t)G_SETGEOMETRYMODE:
            rsp.geometry_mode |= cmd->words.w1;
            DTRACE("G_SETGEOMETRYMODE |= %08x -> %08x", cmd->words.w1, rsp.geometry_mode);
            break;
        case (uint8_t)G_CLEARGEOMETRYMODE:
            rsp.geometry_mode &= ~cmd->words.w1;
            DTRACE("G_CLEARGEOMETRYMODE &= ~%08x -> %08x", cmd->words.w1, rsp.geometry_mode);
            break;
        case (uint8_t)G_TEXTURE: {
            /*
             * gSPTexture(s, t, level, tile, on) — gbi.h packs `level` at bits
             * 11..13 and `on` at bits 0..7 of w0. Both are dropped here.
             *
             * Stock F3D treats `on` as a latched global texture-enable, so
             * ignoring it would be exactly the "per-call vs latched" defect
             * class. It is not one for F3DDKR: the whole game emits G_TEXTURE
             * from ONE place, rcp_dkr.c dRspInit's
             * `gsSPTexture(0, 0, 0, 0, 0)` — s=t=level=tile=0 and on=G_OFF.
             * If the microcode honoured `on`, retail DKR would render entirely
             * untextured; texture enable is instead per-primitive, carried by
             * gSPPolygon's TRIN_ENABLE_TEXTURE bit. Texturing is likewise
             * single-level, so `level` has no consumer.
             * Measured over a 27-run route sweep (179,397 G_TEXTURE commands
             * across intro/hubs/menus/races/attract/credits/Taj/Adventure Two):
             * `on`, `level`, s/t scale and `tile` were ZERO in every single one,
             * i.e. dRspInit is provably the only emitter at runtime too.
             */
            rsp.tex_scale_s = (uint16_t)C1(cmd, 16, 16);
            rsp.tex_scale_t = (uint16_t)C1(cmd, 0, 16);
            rsp.tile_base = (uint8_t)C0(cmd, 8, 3);
            DTRACE("G_TEXTURE scaleS=%04x scaleT=%04x tile_base=%d",
                   rsp.tex_scale_s, rsp.tex_scale_t, rsp.tile_base);
            break;
        }
        case (uint8_t)G_PERSPNORMALIZE:
            break; /* Perspective-normalization scale is irrelevant to HLE. */
        case (uint8_t)G_POPMTX:
            dkr_dl_fault("G_POPMTX is not implemented", cmd, depth);
            break;
        case (uint8_t)G_CULLDL:
            dkr_dl_fault("G_CULLDL is not implemented", cmd, depth);
            break;
        case (uint8_t)G_RDPHALF_1:
        case (uint8_t)G_RDPHALF_2:
            dkr_dl_fault("orphaned RDPHALF command", cmd, depth);
            break;

        /* ---- SP: othermode ---- */
        case (uint8_t)G_SETOTHERMODE_H: {
            uint32_t sft = C0(cmd, 8, 8), len = C0(cmd, 0, 8);
            uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : (((1U << len) - 1) << sft));
            rdp.other_mode_h = (rdp.other_mode_h & ~mask) | (cmd->words.w1 & mask);
            break;
        }
        case (uint8_t)G_SETOTHERMODE_L: {
            uint32_t sft = C0(cmd, 8, 8), len = C0(cmd, 0, 8);
            uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : (((1U << len) - 1) << sft));
            rdp.other_mode_l = (rdp.other_mode_l & ~mask) | (cmd->words.w1 & mask);
            break;
        }
        case G_RDPSETOTHERMODE:  /* gDPSetOtherMode — whole H (24b) + L (32b) */
            rdp.other_mode_h = cmd->words.w0 & 0x00FFFFFF;
            rdp.other_mode_l = cmd->words.w1;
            DTRACE("G_RDPSETOTHERMODE h=%06x l=%08x", rdp.other_mode_h, rdp.other_mode_l);
            break;

        /* ---- RDP: images ---- */
        case G_SETCIMG:
            rdp.color_image_address = dkr_resolve(cmd->words.w1);
            rdp.color_image_token   = cmd->words.w1;
            DTRACE("G_SETCIMG addr=%08x->%p", cmd->words.w1, rdp.color_image_address);
            break;
        case G_SETZIMG:
            rdp.z_buf_address = dkr_resolve(cmd->words.w1);
            rdp.z_buf_token   = cmd->words.w1;
            DTRACE("G_SETZIMG addr=%08x->%p", cmd->words.w1, rdp.z_buf_address);
            break;
        case G_SETTIMG:
            dkr_dp_set_texture_image(C0(cmd, 19, 2), C0(cmd, 0, 12) + 1,
                                     dkr_resolve(cmd->words.w1));
            DTRACE("G_SETTIMG siz=%u width=%u addr=%08x->%p", (unsigned)C0(cmd,19,2),
                   (unsigned)(C0(cmd,0,12)+1), cmd->words.w1, (const void *)rdp.to_load.addr);
            break;

        /* ---- RDP: tiles / texture load ---- */
        case G_SETTILE: {
            uint8_t tl = (uint8_t)C1(cmd, 24, 3);
            dkr_dp_set_tile((uint8_t)C0(cmd, 21, 3), (uint8_t)C0(cmd, 19, 2),
                            C0(cmd, 9, 9), C0(cmd, 0, 9), tl,
                            C1(cmd, 20, 4), C1(cmd, 18, 2), C1(cmd, 14, 4),
                            C1(cmd, 10, 4), C1(cmd, 8, 2), C1(cmd, 4, 4), C1(cmd, 0, 4));
            if (tl < 8)
                /* masks/maskt are traced beside cms/cmt because the RDP's own
                 * clamp decision reads both: a zero mask forces clamping on
                 * that axis whatever the clamp bit says. */
                DTRACE("G_SETTILE tile=%d fmt=%u siz=%u line=%u tmem=%u pal=%u "
                       "cms=%u cmt=%u masks=%u maskt=%u",
                       tl, rdp.tile[tl].fmt, rdp.tile[tl].siz, rdp.tile[tl].line_size_bytes,
                       rdp.tile[tl].tmem, rdp.tile[tl].palette, rdp.tile[tl].cms, rdp.tile[tl].cmt,
                       rdp.tile[tl].masks, rdp.tile[tl].maskt);
            break;
        }
        case G_SETTILESIZE: {
            uint8_t tl = (uint8_t)C1(cmd, 24, 3);
            dkr_dp_set_tile_size(tl, (uint16_t)C0(cmd, 12, 12),
                                 (uint16_t)C0(cmd, 0, 12), (uint16_t)C1(cmd, 12, 12),
                                 (uint16_t)C1(cmd, 0, 12));
            if (tl < 8)
                DTRACE("G_SETTILESIZE tile=%d uls=%u ult=%u lrs=%u lrt=%u -> w=%u h=%u", tl,
                       rdp.tile[tl].uls, rdp.tile[tl].ult, rdp.tile[tl].lrs, rdp.tile[tl].lrt,
                       rdp.tile[tl].width, rdp.tile[tl].height);
            break;
        }
        case G_LOADBLOCK: {
            uint8_t tl = (uint8_t)C1(cmd, 24, 3);
            dkr_dp_load_block(tl, C0(cmd, 12, 12), C0(cmd, 0, 12),
                              C1(cmd, 12, 12), C1(cmd, 0, 12));
            uint32_t sl = (tl < 8 && rdp.tile[tl].tmem < 512) ? rdp.tile[tl].tmem : 0;
            DTRACE("G_LOADBLOCK tile=%d tmem=%u addr=%p size=%u", tl, sl,
                   (const void *)rdp.loaded_texture[sl].addr, rdp.loaded_texture[sl].size_bytes);
            break;
        }
        case G_LOADTILE:
            dkr_dp_load_tile((uint8_t)C1(cmd, 24, 3), C0(cmd, 12, 12), C0(cmd, 0, 12),
                             C1(cmd, 12, 12), C1(cmd, 0, 12));
            DTRACE("G_LOADTILE tile=%u", (unsigned)C1(cmd, 24, 3));
            break;
        case G_LOADTLUT:
            dkr_dp_load_tlut((uint8_t)C1(cmd, 24, 3), C0(cmd, 12, 12), C0(cmd, 0, 12),
                             C1(cmd, 12, 12), C1(cmd, 0, 12));
            DTRACE("G_LOADTLUT tile=%u palfmt=%08x", (unsigned)C1(cmd, 24, 3), rdp.palette_fmt);
            break;

        /* ---- RDP: combine / colours ---- */
        case G_SETCOMBINE:
            dkr_dp_set_combine(cmd);
            DTRACE("G_SETCOMBINE cc_id=%016llx", (unsigned long long)rdp.combine_mode);
            break;
        case G_SETENVCOLOR:
            rdp.env_color = (struct RGBA){ (uint8_t)C1(cmd,24,8), (uint8_t)C1(cmd,16,8),
                                           (uint8_t)C1(cmd,8,8), (uint8_t)C1(cmd,0,8) };
            break;
        case G_SETPRIMCOLOR:
            rdp.prim_lod_fraction = (uint8_t)C0(cmd, 0, 8);
            rdp.authored_prim_color = (struct RGBA){
                (uint8_t)C1(cmd,24,8), (uint8_t)C1(cmd,16,8),
                (uint8_t)C1(cmd,8,8), (uint8_t)C1(cmd,0,8) };
            dkr_replay_apply_primitive_alpha();
            break;
        case G_SETFOGCOLOR:
            rdp.fog_color = (struct RGBA){ (uint8_t)C1(cmd,24,8), (uint8_t)C1(cmd,16,8),
                                           (uint8_t)C1(cmd,8,8), (uint8_t)C1(cmd,0,8) };
            break;
        case G_SETBLENDCOLOR:
            break;  /* blend colour unused by the shader path */
        case G_SETFILLCOLOR: {
            uint32_t c = cmd->words.w1 & 0xffff;  /* RGBA5551 (low 16) */
            uint8_t r = (c >> 11) & 0x1f, g = (c >> 6) & 0x1f, b = (c >> 1) & 0x1f;
            rdp.fill_color = (struct RGBA){ SCALE_5_8(r), SCALE_5_8(g), SCALE_5_8(b),
                                            (uint8_t)((c & 1) ? 255 : 0) };
            break;
        }

        /* ---- RDP: rectangles / scissor ---- */
        case G_FILLRECT:
            DTRACE("G_FILLRECT ul=(%d,%d) lr=(%d,%d) fill=%02x%02x%02x%02x",
                   (int32_t)C1(cmd,12,12), (int32_t)C1(cmd,0,12),
                   (int32_t)C0(cmd,12,12), (int32_t)C0(cmd,0,12),
                   rdp.fill_color.r, rdp.fill_color.g, rdp.fill_color.b, rdp.fill_color.a);
            dkr_begin_primitive(
                rsp.draw_space != G_MTX_DKR_SPACE_WORLD ||
                !((int32_t)C1(cmd,12,12) <= 0 &&
                  (int32_t)C1(cmd,0,12) <= 0 &&
                  (int32_t)C0(cmd,12,12) >=
                      DESIRED_SCREEN_WIDTH * 4 - 4 &&
                  (int32_t)C0(cmd,0,12) >=
                      DESIRED_SCREEN_HEIGHT * 4 - 4));
            dkr_dp_fill_rectangle((int32_t)C1(cmd,12,12), (int32_t)C1(cmd,0,12),
                                  (int32_t)C0(cmd,12,12), (int32_t)C0(cmd,0,12));
            break;
        case G_SETSCISSOR:
            dkr_dp_set_scissor(C0(cmd,12,12), C0(cmd,0,12), C1(cmd,12,12), C1(cmd,0,12));
            DTRACE("G_SETSCISSOR -> {x=%d y=%d w=%d h=%d}", rdp.scissor.x, rdp.scissor.y,
                   rdp.scissor.width, rdp.scissor.height);
            break;
        case G_TEXRECT:
        case G_TEXRECTFLIP: {
            int32_t lrx = (int32_t)C0(cmd, 12, 12);
            int32_t lry = (int32_t)C0(cmd, 0, 12);
            uint8_t tile = (uint8_t)C1(cmd, 24, 3);
            int32_t ulx = (int32_t)C1(cmd, 12, 12);
            int32_t uly = (int32_t)C1(cmd, 0, 12);
            /* Two trailing RDPHALF words carry s/t and dsdx/dtdy. */
            if ((limit > 0 && (cmd - start) + 2 >= limit) ||
                dkr_arena_room(cmd) < sizeof(Gfx) * 3) {
                dkr_dl_fault("truncated texture rectangle", cmd, depth);
                return;
            }
            Gfx *h1 = cmd + 1;
            Gfx *h2 = cmd + 2;
            if ((uint8_t) C0(h1, 24, 8) != (uint8_t) G_RDPHALF_1 ||
                (uint8_t) C0(h2, 24, 8) != (uint8_t) G_RDPHALF_2) {
                dkr_dl_fault("texture rectangle has invalid half commands",
                             cmd, depth);
                return;
            }
            if (census) {
                s_dl_census_commands += 2;
                s_dl_census_opcodes[(uint8_t)G_RDPHALF_1]++;
                s_dl_census_opcodes[(uint8_t)G_RDPHALF_2]++;
            }
            int16_t uls = (int16_t)C1(h1, 16, 16), ult = (int16_t)C1(h1, 0, 16);
            int16_t dsdx = (int16_t)C1(h2, 16, 16), dtdy = (int16_t)C1(h2, 0, 16);
            DTRACE("G_TEXRECT%s ul=(%d,%d) lr=(%d,%d) tile=%u s=%d t=%d dsdx=%d dtdy=%d",
                   op == (uint8_t)G_TEXRECTFLIP ? "FLIP" : "", ulx, uly, lrx, lry,
                   tile, uls, ult, dsdx, dtdy);
            dkr_begin_primitive(true);
            dkr_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy,
                                     op == (uint8_t)G_TEXRECTFLIP);
            cmd += 2;  /* consume the two RDPHALF commands */
            break;
        }

        /* ---- RDP: syncs (no-ops) ---- */
        case G_RDPFULLSYNC:
        case G_RDPTILESYNC:
        case G_RDPPIPESYNC:
        case G_RDPLOADSYNC:
        case G_NOOP:
            break;

        default:
            dkr_dl_fault("unknown display-list opcode", cmd, depth);
            break;
        }
        cmd++;
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void gfx_dkr_report_dl_census(void) {
    if (s_dl_census_reported || !dkr_dl_census_enabled()) {
        return;
    }
    s_dl_census_reported = true;
    fprintf(stderr,
            "[DL-CENSUS] commands=%llu lists=%llu maxDepth=%u faults=%llu "
            "opcodes=",
            (unsigned long long)s_dl_census_commands,
            (unsigned long long)s_dl_census_lists,
            s_dl_census_max_depth,
            (unsigned long long)s_dl_census_faults);
    bool first = true;
    for (unsigned opcode = 0; opcode < 256; opcode++) {
        if (s_dl_census_opcodes[opcode] == 0) {
            continue;
        }
        fprintf(stderr, "%s%02x:%llu", first ? "" : ",", opcode,
                (unsigned long long)s_dl_census_opcodes[opcode]);
        first = false;
    }
    fprintf(stderr, "\n");
}

bool gfx_init(struct GfxRenderingAPI *rapi) {
    if (rapi == NULL || rapi->init == NULL) {
        gfx_rapi = NULL;
        return false;
    }
    if (!rapi->init()) {
        /* Backend init is allowed to acquire resources before a later step
         * fails. Give it the same deterministic partial-init cleanup contract
         * as a fully initialized backend. */
        if (rapi->shutdown != NULL) {
            rapi->shutdown();
        }
        gfx_rapi = NULL;
        return false;
    }
    gfx_rapi = rapi;
    return true;
}

void gfx_shutdown(void) {
    uint64_t created;
    uint64_t deleted;
    uint64_t shaders;
    uint32_t live_before;
    struct GfxRenderingAPI *rapi = gfx_rapi;

    if (rapi == NULL) {
        return;
    }

    /*
     * Capacity/content reports consume live shader metadata, so publish them
     * before the backend frees that metadata. Both reporters are internally
     * one-shot and opt-in where appropriate.
     */
    gfx_dkr_report_dl_census();
#ifdef MDKR_WEBGPU_BACKEND
    if (rapi == &gfx_webgpu_api) {
        gfx_webgpu_report_limits();
    }
#endif

    created = gfx_dkr_texture_ids_created;
    shaders = gfx_dkr_shader_programs_created;
    live_before = gfx_dkr_texture_ids_live;

    /* Frontend texture ids are backend children. Delete them while the device
     * and native GL context are still live, then drop any selected shader
     * before the backend destroys its program pool. */
    for (int i = 0; i < DKR_TEXCACHE_SIZE; i++) {
        dkr_texcache_delete_slot(i);
    }
    if (rendering_state.shader_program != NULL &&
        rapi->unload_shader != NULL) {
        rapi->unload_shader(rendering_state.shader_program);
    }
    rendering_state.shader_program = NULL;
    deleted = gfx_dkr_texture_ids_deleted;

    if (rapi->shutdown != NULL) {
        rapi->shutdown();
    }
    gfx_rapi = NULL;
    free(tex_decode_buf);
    tex_decode_buf = NULL;
    tex_decode_cap = 0;
    free(font_sdf_buf);
    font_sdf_buf = NULL;
    font_sdf_cap = 0;
    free(tex_row_buf);
    tex_row_buf = NULL;
    tex_row_cap = 0;
    free(tex_mip_buf);
    tex_mip_buf = NULL;
    tex_mip_cap = 0;
    gfx_presentation_packet_shutdown();
    gfx_retained_task_shutdown();
    gfx_shadow_frame_shutdown();
    gfx_reset_renderer_caches();
    /* Every field is READ back from the variable that owns it, so a clean
     * teardown prints zeros and a missed release prints what survived.
     * backendReleased reflects the actual vtable pointer; cpuScratch is the
     * total capacity still held by the frontend's decode/row/mip/SDF buffers. */
    fprintf(stderr,
            "[GFX-SHUTDOWN] texturesCreated=%llu texturesDeleted=%llu "
            "live=%u->%u shaders=%llu backendReleased=%d cpuScratch=%llu\n",
            (unsigned long long)created,
            (unsigned long long)deleted,
            live_before, gfx_dkr_texture_ids_live,
            (unsigned long long)shaders,
            gfx_rapi == NULL,
            (unsigned long long)(tex_decode_cap + font_sdf_cap +
                                 tex_row_cap + tex_mip_cap));
}

void gfx_reset_renderer_caches(void) {
    /*
     * Texture ids and ShaderProgram pointers are backend-owned. A replacement
     * WebGPU device or a live WebGPU→GL switch invalidates both classes even
     * though the CPU-side decoded source data remains valid. Clearing only
     * these regenerable frontend caches makes the next display list rebuild
     * them without touching simulation state.
     */
    memset(tex_cache, 0, sizeof(tex_cache));
    tex_cache_next = 0;
    gfx_dkr_texture_ids_created = 0;
    gfx_dkr_texture_ids_deleted = 0;
    gfx_dkr_texture_ids_live = 0;
    gfx_dkr_texture_ids_high_water = 0;
    gfx_dkr_shader_programs_created = 0;
    memset(&dkr_resource_generation, 0, sizeof(dkr_resource_generation));
    memset(cc_pool, 0, sizeof(cc_pool));
    cc_pool_size = 0;
    memset(&rendering_state, 0, sizeof(rendering_state));
    rendering_state.depth_mode = 0xFF;
    rendering_state.blend_mode = (enum GfxBlendMode)0xFF;
    buf_vbo_len = 0;
    buf_vbo_num_tris = 0;
}

bool gfx_rebind_renderer(struct GfxRenderingAPI *rapi) {
    gfx_reset_renderer_caches();
    if (!gfx_init(rapi)) {
        return false;
    }
    if (gfx_rapi->on_resize != NULL) {
        gfx_rapi->on_resize();
    }
    return true;
}

void gfx_dkr_set_logical_surface(uint32_t width, uint32_t height) {
    /*
     * Ignore an implausible surface rather than divide the whole presentation
     * mapping by it: this arrives from game state and a zero would take every
     * mapped rectangle with it.
     */
    if (width == 0u || height == 0u || width > 4096u || height > 4096u) {
        return;
    }
    if ((float)width == dkr_logical_width &&
        (float)height == dkr_logical_height) {
        return;
    }
    dkr_logical_width = (float)width;
    dkr_logical_height = (float)height;
    /* The retained logical viewport/scissor now mean something different. */
    dkr_remap_viewport_and_scissor();
}

void gfx_set_dimensions(uint32_t width, uint32_t height) {
    uint32_t max_dimension = 0;
    bool output_changed;
    bool render_changed;

    if (width == 0) width = DESIRED_SCREEN_WIDTH;
    if (height == 0) height = DESIRED_SCREEN_HEIGHT;

#ifdef MDKR_WEBGPU_BACKEND
    if (mdkr_render_backend() == MDKR_BACKEND_WEBGPU) {
        max_dimension = (uint32_t)gfx_webgpu_max_offscreen_dim();
    }
#endif
#ifndef __EMSCRIPTEN__
    if (mdkr_render_backend() == MDKR_BACKEND_GL) {
        max_dimension = (uint32_t)gfx_opengl_max_offscreen_dim();
    }
#endif
    if (max_dimension > 0 && (width > max_dimension || height > max_dimension)) {
        double scale_x = (double)max_dimension / (double)width;
        double scale_y = (double)max_dimension / (double)height;
        double scale = scale_x < scale_y ? scale_x : scale_y;
        width = (uint32_t)floor((double)width * scale);
        height = (uint32_t)floor((double)height * scale);
        if (width == 0) width = 1;
        if (height == 0) height = 1;
    }

    /*
     * Video.RenderScale: render offscreen at width*scale and let the backend's
     * resolve shrink it to the window. Everything downstream of here -- the
     * display layout, viewports, scissors and the backend's scene target --
     * works in this scaled RENDER space, and the resolve is the single place it
     * returns to output space. That is why the multiply belongs here and not in
     * a backend: scaling a scene target without scaling the viewports that draw
     * into it just renders the frame into one corner.
     *
     * GL resolves with its end-of-frame glBlitFramebuffer(scene -> drawable,
     * GL_LINEAR) and reads back the resolved default framebuffer. WebGPU
     * resolves with wgpu_run_resolve() into an output-sized texture that the
     * present copy and readback both read, so frame dumps stay output-sized on
     * both backends.
     */
    output_changed =
        width != gfx_output_dimensions.width ||
        height != gfx_output_dimensions.height;
    gfx_output_dimensions.width = width;
    gfx_output_dimensions.height = height;
    gfx_output_dimensions.aspect_ratio = (float)width / (float)height;

    {
        unsigned int scaled_w;
        unsigned int scaled_h;
        gfx_render_scaled_dimensions(
            width, height, max_dimension, &scaled_w, &scaled_h);
        width = scaled_w;
        height = scaled_h;
    }

    render_changed =
        width != gfx_current_dimensions.width ||
        height != gfx_current_dimensions.height;

    /* Report OUTPUT and RENDER resolution explicitly. Camera projection is
     * keyed to OUTPUT below: render-scale rounding and texture caps may change
     * internal pixels, but must never change the world-space lens. */
    if (output_changed || render_changed) {
        double applied_scale_w =
            (double)width / (double)gfx_output_dimensions.width;
        double applied_scale_h =
            (double)height / (double)gfx_output_dimensions.height;
        double applied_scale =
            applied_scale_w < applied_scale_h
                ? applied_scale_w
                : applied_scale_h;
        printf("[DISPLAY] output=%ux%u render=%ux%u scale=%.2f "
               "effectiveScale=%.2f\n",
               gfx_output_dimensions.width, gfx_output_dimensions.height,
               width, height,
               (double)gfx_effective_render_scale(),
               applied_scale);
    }
    gfx_current_dimensions.width = width;
    gfx_current_dimensions.height = height;
    gfx_current_dimensions.aspect_ratio = (float)width / (float)height;
    mdkr_display_set_dimensions(
        gfx_output_dimensions.width, gfx_output_dimensions.height);

    if (render_changed && gfx_rapi && gfx_rapi->on_resize) {
        gfx_rapi->on_resize();
    }
}

bool gfx_get_capture_dimensions(uint32_t *width, uint32_t *height) {
    if (width == NULL || height == NULL) {
        return false;
    }
#ifdef MDKR_WEBGPU_BACKEND
    if (mdkr_render_backend() == MDKR_BACKEND_WEBGPU) {
        int output_width = 0;
        int output_height = 0;
        if (!gfx_webgpu_get_output_size(&output_width, &output_height) ||
            output_width <= 0 || output_height <= 0) {
            return false;
        }
        *width = (uint32_t)output_width;
        *height = (uint32_t)output_height;
        return true;
    }
#endif
    if (gfx_output_dimensions.width == 0 ||
        gfx_output_dimensions.height == 0) {
        return false;
    }
    *width = gfx_output_dimensions.width;
    *height = gfx_output_dimensions.height;
    return true;
}

bool gfx_start_frame(uint64_t authored_tick) {
    if (gfx_rapi == NULL || gfx_rapi->start_frame == NULL ||
        !gfx_rapi->start_frame()) {
        return false;
    }
    gfx_shadow_capture_begin();
    gfx_dkr_reset_interpreter_state();
    dkr_last_walked_authored_tick = authored_tick;
    if (present_sched_replay_armed()) {
        (void)gfx_retained_task_capture_begin(
            authored_tick, g_dkrArenaBase, g_dkrArenaSize);
        /* Billboard ownership was registered while game code built the list.
         * Deformed model vertices only become visible when HLE resolves G_VTX,
         * so begin their independent tick-stamped capture at the real walk. */
        gfx_presentation_packet_capture_begin(authored_tick);
        /* The exact state this walk is about to start from — see
         * dkr_walk_entry_rdp. */
        memcpy(dkr_walk_entry_rdp, &rdp, sizeof(rdp));
        memcpy(dkr_walk_entry_rsp, &rsp, sizeof(rsp));
        memcpy(dkr_walk_entry_segments, gfx_segment_table,
               sizeof(dkr_walk_entry_segments));
        dkr_walk_entry_texrect = dkr_in_texrect;
        dkr_walk_entry_valid = true;
    }
    return true;
}

/*
 * The per-frame interpreter reset, without the backend start_frame or the
 * caster capture_begin. A presentation replay needs exactly this half: the
 * matrix/RDP/viewport state must start from the same place the real walk
 * started from, but the caster frame was already begun, filled and committed by
 * that real walk and must not be reopened.
 */
void gfx_dkr_reset_interpreter_state(void) {
    /* Reset per-frame interpreter state, matrices included. DKR rebuilds its
     * whole display list every frame, matrix commands and all, so no RSP state
     * may carry across a frame boundary. */
    memset(&rsp, 0, sizeof(rsp));
    dkr_output_overlay_active = false;
    dkr_output_overlay_suppressed = false;
    dkr_output_overlay_frame_draws = 0;
    dkr_output_overlay_late_world_draws = 0;
    dkr_shadow_receiver_view = -1;
    dkr_primitive_serial = 0;
    dkr_last_world_primitive = 0;
    dkr_primitive_prepared = false;
    dkr_primitive_overlay_candidate = false;
    rsp.draw_space = G_MTX_DKR_SPACE_WORLD;
    rsp.tex_scale_s = rsp.tex_scale_t = 0xFFFF;
    rsp.mtx[0][0][0] = rsp.mtx[0][1][1] = rsp.mtx[0][2][2] = rsp.mtx[0][3][3] = 1.0f;
    rsp.mtx[1][0][0] = rsp.mtx[1][1][1] = rsp.mtx[1][2][2] = rsp.mtx[1][3][3] = 1.0f;
    rsp.mtx[2][0][0] = rsp.mtx[2][1][1] = rsp.mtx[2][2][2] = rsp.mtx[2][3][3] = 1.0f;

    /* Seed sensible RDP defaults so draws before explicit setup still work. */
    rdp.prim_color = rdp.authored_prim_color = rdp.env_color =
        (struct RGBA){ 255, 255, 255, 255 };
    rdp.other_mode_h = 0;
    rdp.other_mode_l = 0;
    rdp.combine_mode = (uint64_t)color_comb(0, 0, 0, G_CCMUX_SHADE) |
                       ((uint64_t)alpha_comb(0, 0, 0, G_ACMUX_SHADE) << 16);

    rdp.logical_viewport =
        (struct FloatXYWidthHeight){ 0.0f, 0.0f, DESIRED_SCREEN_WIDTH, DESIRED_SCREEN_HEIGHT };
    rdp.logical_scissor = rdp.logical_viewport;
    rdp.logical_viewport_valid = true;
    rdp.logical_scissor_valid = true;
    dkr_remap_viewport_and_scissor();

    /* Invalidate cached backend state so it is re-applied this frame. */
    memset(&rendering_state, 0, sizeof(rendering_state));
    rendering_state.depth_mode = 0xFF;
    rendering_state.blend_mode = (enum GfxBlendMode)0xFF;

    buf_vbo_len = 0;
    buf_vbo_num_tris = 0;
}

void gfx_run(Gfx *dl) {
    if (!gfx_rapi) return;
    /* A replay re-walks a list this counter already advanced for. Bumping it
     * again would desynchronise MDKR_DL_FRAME filtering and every trace that
     * keys off the frame index from the dumped-frame numbering. */
    if (!dkr_replay_pass) {
        dkr_frame_index++;
        dkr_last_walked_dl = dl;
    }
    dkr_trace_this_frame = dkr_dl_trace_on();
    dkr_dbg_emitted = dkr_dbg_onscreen = 0;
    dkr_dbg_clipped = dkr_dbg_clip_dropped = dkr_dbg_clip_degen = 0;
    if (dkr_native_ui_enabled() &&
        gfx_rapi != NULL && gfx_rapi->begin_output_overlay != NULL &&
        (g_pcRemasterFX ||
         gfx_current_dimensions.width != gfx_output_dimensions.width ||
         gfx_current_dimensions.height != gfx_output_dimensions.height)) {
        uintptr_t saved_segments[16];
        DkrOverlayScan scan = {
            G_MTX_DKR_SPACE_WORLD,
            0,
            0
        };
        memcpy(saved_segments, gfx_segment_table, sizeof(saved_segments));
        dkr_scan_overlay_order(dl, 0, 0, &scan);
        memcpy(gfx_segment_table, saved_segments, sizeof(saved_segments));
        dkr_last_world_primitive = scan.last_world;
    }
    DTRACE("==== frame %d gfx_run dl=%p ====", dkr_frame_index, (void *)dl);
    dkr_run_dl(dl, 0, 0);
    gfx_flush();
    if (mdkr_trace_level() >= 2)
        mdkr_trace("gfx_run frame %d: emitted=%d onscreen=%d nearclip=%d dropped=%d degen=%d",
                   dkr_frame_index, dkr_dbg_emitted, dkr_dbg_onscreen,
                   dkr_dbg_clipped, dkr_dbg_clip_dropped, dkr_dbg_clip_degen);
    dkr_trace_this_frame = false;
}

void gfx_run_dl(Gfx *dl) { gfx_run(dl); }

void gfx_end_frame(void) {
    gfx_flush();
    if (gfx_rapi && gfx_rapi->end_frame) gfx_rapi->end_frame();
    if (gfx_rapi && gfx_rapi->finish_render) gfx_rapi->finish_render();
    gfx_shadow_capture_commit();
    /*
     * The registry and the projected-range/excluded-caster marks live only
     * from display-list BUILD time to this reset, so this is the last instant
     * a presentation replay can obtain them (a bare second gfx_run finds an
     * empty registry). Freeze before the resets,
     * and only when a replay is actually armed, so the shipping path keeps
     * paying nothing.
     */
    if (present_sched_replay_armed()) {
        const uint64_t perf_freeze = present_perf_now();
        (void)gfx_retained_task_capture_commit(
            dkr_last_walked_dl, dkr_walk_entry_segments);
        gfx_presentation_packet_freeze();
        gfx_shadow_replay_freeze();
        present_perf_add(PRESENT_PERF_FREEZE, perf_freeze);
    }
    gfx_shadow_projected_ranges_reset();
    gfx_shadow_matrix_registry_reset();
    if (getenv("MDKR_UI_OVERLAY_TRACE") != NULL) {
        fprintf(stderr,
                "[UI-3] frame=%d active=%d draws=%u lateWorld=%u "
                "primitives=%llu lastWorld=%llu render=%ux%u output=%ux%u "
                "total=%llu beginFailures=%llu\n",
                dkr_frame_index,
                dkr_output_overlay_active ? 1 : 0,
                dkr_output_overlay_frame_draws,
                dkr_output_overlay_late_world_draws,
                (unsigned long long)dkr_primitive_serial,
                (unsigned long long)dkr_last_world_primitive,
                gfx_current_dimensions.width,
                gfx_current_dimensions.height,
                gfx_output_dimensions.width,
                gfx_output_dimensions.height,
                (unsigned long long)dkr_output_overlay_frames,
                (unsigned long long)dkr_output_overlay_begin_failures);
    }
}

bool gfx_renderer_failed(void) {
    return gfx_rapi != NULL && gfx_rapi->get_status != NULL &&
           gfx_rapi->get_status() == GFX_RENDERING_FATAL;
}

/* ------------------------------------------------------------------------- */
/* Presentation replay walk                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Re-walk the tick's own display list to produce another image from it.
 *
 * This is deliberately NOT a re-invocation of the game's render tree. Calling
 * render_scene()/mode_game_render() again would re-run everything embedded in
 * it — ttcam_update()'s cross-frame spectate smoothing among them — and would
 * advance simulation state during presentation. Re-walking the already-built list at the
 * HLE layer never re-enters game/src at all, so that whole class of
 * render-embedded state advance is avoided structurally rather than audited
 * case by case.
 *
 * The image is produced into the same target the real walk used: the backend's
 * start_frame clears it and the identical command stream redraws it. With
 * `overrides == NULL` the redraw is the same picture; with an interpolated
 * camera view-projection it is the same scene from a moved camera.
 */
static bool gfx_dkr_replay_walk_impl(
    const GfxShadowReplayViewProjection *overrides, size_t override_count,
    bool object_alpha_valid, uint64_t numerator, uint64_t denominator) {
    bool completed = false;
    bool retained_entered = false;
    bool live_arena_poisoned = false;
    void *live_arena = NULL;
    uint32_t live_arena_size = 0u;
    uint8_t *live_arena_backup = NULL;
    GfxRetainedTaskView retained_task;

    if (gfx_rapi == NULL || dkr_last_walked_dl == NULL || dkr_replay_pass ||
        !dkr_walk_entry_valid || !gfx_presentation_packet_frozen()) {
        return false;
    }
    if (!gfx_retained_task_acquire(
            dkr_last_walked_authored_tick, &retained_task)) {
        return false;
    }
    dkr_replay_force_recompose = present_sched_test_force_recompose();
    if (!gfx_shadow_replay_restore(overrides, override_count)) {
        return false;
    }
    dkr_replay_pass = true;
    dkr_replay_dependency_failed = false;
    dkr_replay_object_alpha_valid =
        object_alpha_valid && dkr_replay_object_interpolation_enabled() &&
        denominator != 0u && numerator < denominator;
    dkr_replay_object_alpha_numerator = numerator;
    dkr_replay_object_alpha_denominator = denominator != 0u ? denominator : 1u;
    /* Caster capture belongs to the real walk, which already committed this
     * tick's frame; a second capture would republish it mid-present. */
    gfx_shadow_capture_suppress(true);

    /* From this point through the HLE walk, every arena token and direct
     * command traversal belongs to the retained authored task. The live arena
     * is restored before releasing the shadow registry, and no game code runs
     * while the private image is installed. */
    live_arena = g_dkrArenaBase;
    live_arena_size = g_dkrArenaSize;
    if (dkr_test_live_arena_poison_enabled()) {
        live_arena_backup = (uint8_t *)malloc(live_arena_size);
        if (live_arena_backup == NULL) {
            goto replay_cleanup;
        }
        memcpy(live_arena_backup, live_arena, live_arena_size);
        memset(live_arena, 0xa5, live_arena_size);
        live_arena_poisoned = true;
    }
    g_dkrArenaBase = (void *)retained_task.retained_arena;
    g_dkrArenaSize = (uint32_t)retained_task.arena_size;
    retained_entered = true;

    if (gfx_rapi->start_frame == NULL || !gfx_rapi->start_frame()) {
        goto replay_cleanup;
    }
    gfx_dkr_reset_interpreter_state();
    /* Start from bit-identical HLE state to the real walk, not from the state
     * the real walk left behind (see dkr_walk_entry_rdp). */
    memcpy(&rdp, dkr_walk_entry_rdp, sizeof(rdp));
    memcpy(&rsp, dkr_walk_entry_rsp, sizeof(rsp));
    memcpy(gfx_segment_table, retained_task.segments,
           sizeof(retained_task.segments));
    dkr_in_texrect = dkr_walk_entry_texrect;
    gfx_run((Gfx *)retained_task.display_list);
    gfx_flush();
    if (gfx_rapi->end_frame) {
        gfx_rapi->end_frame();
    }
    if (gfx_rapi->finish_render) {
        gfx_rapi->finish_render();
    }
    completed = !dkr_replay_dependency_failed;

replay_cleanup:
    if (retained_entered) {
        g_dkrArenaBase = live_arena;
        g_dkrArenaSize = live_arena_size;
    }
    if (live_arena_poisoned) {
        memcpy(live_arena, live_arena_backup, live_arena_size);
        if (completed) {
            gfx_retained_task_note_live_arena_poison();
        }
    }
    free(live_arena_backup);
    gfx_shadow_capture_suppress(false);
    /* Put back whatever the game had registered when the replay displaced it:
     * by the time a presentation replay runs, the NEXT tick's display list has
     * already been built and registered (gfx_shadow_replay_release). */
    (void)gfx_shadow_replay_release();
    dkr_replay_object_alpha_valid = false;
    dkr_replay_object_alpha_numerator = 0;
    dkr_replay_object_alpha_denominator = 1;
    dkr_replay_pass = false;
    dkr_replay_dependency_failed = false;
    if (completed) {
        dkr_replay_walks++;
    }
    return completed;
}

bool gfx_dkr_replay_walk(
    const GfxShadowReplayViewProjection *overrides, size_t override_count) {
    return gfx_dkr_replay_walk_impl(overrides, override_count, false, 0u, 1u);
}

bool gfx_dkr_replay_walk_interpolated(
    const GfxShadowReplayViewProjection *overrides, size_t override_count,
    uint64_t numerator, uint64_t denominator) {
    return gfx_dkr_replay_walk_impl(overrides, override_count, true, numerator,
                                    denominator);
}

void gfx_dkr_replay_invalidate(void) {
    dkr_last_walked_dl = NULL;
    dkr_last_walked_authored_tick = 0u;
    dkr_future_last_published_tick = UINT64_MAX;
    dkr_walk_entry_valid = false;
    gfx_presentation_packet_invalidate();
    gfx_retained_task_invalidate();
}

uint64_t gfx_dkr_real_walk_count(void) {
    return (uint64_t)(dkr_frame_index + 1);
}

uint64_t gfx_dkr_last_walked_authored_tick(void) {
    return dkr_last_walked_authored_tick;
}

void gfx_dkr_replay_get_stats(
    uint64_t *walks, uint64_t *matrix_hits, uint64_t *matrix_misses,
    uint64_t *matrix_rejects, uint64_t *real_walks) {
    if (walks != NULL) {
        *walks = dkr_replay_walks;
    }
    if (real_walks != NULL) {
        /* Real gfx_run walks, i.e. graphics tasks. DKR does not submit one on
         * every frame, so this — not the tick count — is what the replay count
         * must equal. */
        *real_walks = (uint64_t)(dkr_frame_index + 1);
    }
    if (matrix_hits != NULL) {
        *matrix_hits = dkr_replay_matrix_hits;
    }
    if (matrix_misses != NULL) {
        *matrix_misses = dkr_replay_matrix_misses;
    }
    if (matrix_rejects != NULL) {
        *matrix_rejects = dkr_replay_matrix_rejects;
    }
}

void gfx_dkr_replay_get_object_stats(uint64_t *hits, uint64_t *holds) {
    if (hits != NULL) {
        *hits = dkr_replay_object_hits;
    }
    if (holds != NULL) {
        *holds = dkr_replay_object_holds;
    }
}

void gfx_dkr_replay_get_billboard_stats(
    uint64_t *matrix_hits, uint64_t *matrix_holds,
    uint64_t *vertex_hits, uint64_t *vertex_holds) {
    if (matrix_hits != NULL) {
        *matrix_hits = dkr_replay_billboard_matrix_hits;
    }
    if (matrix_holds != NULL) {
        *matrix_holds = dkr_replay_billboard_matrix_holds;
    }
    if (vertex_hits != NULL) {
        *vertex_hits = dkr_replay_billboard_vertex_hits;
    }
    if (vertex_holds != NULL) {
        *vertex_holds = dkr_replay_billboard_vertex_holds;
    }
}

/*
 * The tolerance's own evidence, always accumulated (these are counters, not
 * clock reads) so a gate can assert the separation margin without arming the
 * MDKR_PRESENT_PERF census.
 *
 * `tolerant_worst` is the largest mismatch the tolerance ACCEPTED and
 * `reject_least` the smallest it did NOT. The gap between them is the live
 * margin: if a future change ever lets those two approach each other, the
 * threshold has stopped sitting in empty space and the assertion goes red
 * before anything is silently swallowed. `reject_least_valid` is false when a
 * run rejected nothing measurable, which a gate must not read as "zero".
 */
uint64_t gfx_dkr_shadow_stale_tenant_count(void) {
    return dkr_shadow_stale_tenants;
}

void gfx_dkr_replay_get_reject_stats(
    uint64_t *tolerant, uint64_t *tolerant_worst, uint64_t *reject_least,
    bool *reject_least_valid) {
    if (tolerant != NULL) {
        *tolerant = dkr_replay_matrix_tolerant;
    }
    if (tolerant_worst != NULL) {
        *tolerant_worst = dkr_replay_matrix_tolerant_worst;
    }
    if (reject_least != NULL) {
        *reject_least = dkr_replay_matrix_reject_least;
    }
    if (reject_least_valid != NULL) {
        *reject_least_valid = dkr_replay_matrix_reject_least_set;
    }
}

/* Backend-agnostic framebuffer readback (M4.5). Delegates to the active
 * backend's read_framebuffer_rgb, which returns bottom-left-origin RGB. Used by
 * the platform frame-dump path for backends (WebGPU) that render offscreen and
 * cannot be captured with glReadPixels. Returns false if the backend has no
 * readback (call sites must guard, like the vtable slot itself). */
int gfx_read_framebuffer_rgb(int x, int y, int width, int height, uint8_t *rgb_out) {
    if (gfx_rapi && gfx_rapi->read_framebuffer_rgb) {
        return gfx_rapi->read_framebuffer_rgb(x, y, width, height, rgb_out) ? 1 : 0;
    }
    return 0;
}
