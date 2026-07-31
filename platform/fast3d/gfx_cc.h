/**
 * gfx_cc.h — Color combiner ID packing and feature extraction.
 * Based on Emill/n64-fast3d-engine (MIT license).
 * Extended for 2-cycle support based on fgsfdsfgs/perfect_dark port.
 */
#ifndef GFX_CC_H
#define GFX_CC_H

#include <stdint.h>
#include <stdbool.h>

/* CC_* intermediate enum — used only in shader_input_mapping VBO packing
 * to identify which CPU-side color source to read for each shader input.
 * These are stored in ColorCombiner.shader_input_mapping[][]. */
enum {
    CC_0,
    CC_TEXEL0,
    CC_TEXEL1,
    CC_PRIM,
    CC_SHADE,
    CC_ENV,
    CC_TEXEL0A,
    CC_LOD
};

/* SHADER_* enum — values packed into shader_id0 (4 bits per slot).
 * These determine what GLSL expression each combiner slot maps to.
 * Must match shader_item_to_str cases in gfx_opengl.c. */
enum {
    SHADER_0,
    SHADER_INPUT_1,
    SHADER_INPUT_2,
    SHADER_INPUT_3,
    SHADER_INPUT_4,
    SHADER_INPUT_5,
    SHADER_INPUT_6,
    SHADER_INPUT_7,
    SHADER_TEXEL0,      /* 8 */
    SHADER_TEXEL0A,     /* 9 */
    SHADER_TEXEL1,      /* 10 */
    SHADER_TEXEL1A,     /* 11 */
    SHADER_1,           /* 12 */
    SHADER_COMBINED,    /* 13 */
    SHADER_NOISE,       /* 14 */
    SHADER_LOD_FRAC     /* 15 — not used in shader_item_to_str; routed via SHADER_INPUT_N */
};

/* Shader option flags — stored in separate uint32_t shader_id1,
 * NOT packed into shader_id0. */
#define SHADER_OPT_ALPHA         (1 << 0)
#define SHADER_OPT_FOG           (1 << 1)
#define SHADER_OPT_TEXTURE_EDGE  (1 << 2)
#define SHADER_OPT_NOISE         (1 << 3)
#define SHADER_OPT_2CYC          (1 << 4)
#define SHADER_OPT_WORLD_POS     (1u << 5)   /* W1.E2: per-vertex world position attribute */
#define SHADER_OPT_SUN_SHADOW    (1u << 6)   /* W1.E3: sun-shadow-map receiver (3x3 PCF) */
#define SHADER_OPT_DFDX_LIGHT    (1u << 7)   /* RL-5: smooth-normal directional sun (legacy bit name) */
#define SHADER_OPT_TEXEL0_CLAMP_S (1 << 8)
#define SHADER_OPT_TEXEL0_CLAMP_T (1 << 9)
#define SHADER_OPT_TEXEL1_CLAMP_S (1 << 10)
#define SHADER_OPT_TEXEL1_CLAMP_T (1 << 11)
#define SHADER_OPT_TEXEL0_N64_FILTER (1 << 12)
#define SHADER_OPT_TEXEL1_N64_FILTER (1 << 13)
#define SHADER_OPT_NOPERSPECTIVE_TEXCOORDS (1 << 14)
#define SHADER_OPT_NOPERSPECTIVE_INPUTS (1 << 15)
#define SHADER_OPT_NOPERSPECTIVE_FOG (1 << 16)
#define SHADER_OPT_DIAG_COLOR_SCALE (1 << 17)
#define SHADER_OPT_N64_FILTER_ALWAYS_3POINT (1 << 18)
#define SHADER_OPT_DIAG_ALPHA_FROM_TEX_INTENSITY (1 << 19)
#define SHADER_OPT_DIAG_XLU_COVERAGE_WRAP_THIN (1 << 20)
#define SHADER_OPT_DIAG_RDP_MEMORY_BLEND (1 << 21)
#define SHADER_OPT_DIAG_RDP_CVG_MEMORY_BLEND (1 << 22)
#define SHADER_OPT_DIAG_ALPHA_SCALE (1 << 23)
#define SHADER_OPT_ROOM_WATER_ALPHA_SUPPRESS (1 << 24)
#define SHADER_OPT_TEXEL0_MASK_S (1u << 25)
#define SHADER_OPT_TEXEL0_MASK_T (1u << 26)
#define SHADER_OPT_TEXEL1_MASK_S (1u << 27)
#define SHADER_OPT_TEXEL1_MASK_T (1u << 28)

struct CCFeatures {
    uint8_t c[2][2][4];       /* [cycle][color_or_alpha][component A/B/C/D] */
    bool opt_alpha;
    bool opt_fog;
    bool opt_texture_edge;
    bool opt_noise;
    bool opt_2cyc;
    bool used_textures[2];
    bool clamp[2][2];         /* [texture 0/1][S/T] — shader-side UV clamping */
    bool tile_mask[2][2];     /* [texture 0/1][S/T] — shader-side N64 mask wrap */
    bool n64_filter[2];       /* [texture 0/1] — N64-native shader texture filter */
    bool noperspective_texcoords;
    bool noperspective_inputs;
    bool noperspective_fog;
    bool diag_color_scale;
    bool n64_filter_always_3point;
    bool diag_alpha_from_tex_intensity;
    bool diag_xlu_coverage_wrap_thin;
    bool diag_rdp_memory_blend;
    bool diag_rdp_cvg_memory_blend;
    bool diag_alpha_scale;
    bool room_water_alpha_suppress;
    bool opt_world_pos;       /* W1.E2: emit the aWorldPos vertex attribute */
    bool opt_sun_shadow;      /* W1.E3: sun-shadow-map receiver (3x3 PCF) */
    bool opt_dfdx_light;      /* RL-5: smooth-normal, linear-light directional sun */
    int num_inputs;
    bool do_single[2][2];     /* [cycle][color_or_alpha] */
    bool do_multiply[2][2];
    bool do_mix[2][2];
    bool color_alpha_same[2]; /* per cycle */
};

void gfx_cc_get_features(uint64_t shader_id0, uint32_t shader_id1, struct CCFeatures *cc_features);

#endif
