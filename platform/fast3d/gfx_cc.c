/**
 * gfx_cc.c — Color combiner feature extraction.
 * Based on Emill/n64-fast3d-engine (n64-fast3d-engine license — modified
 * BSD-2-Clause; see src/platform/fast3d/PROVENANCE.md).
 * Rewritten for 2-cycle support based on fgsfdsfgs/perfect_dark port.
 */
#include "gfx_cc.h"

void gfx_cc_get_features(uint64_t shader_id0, uint32_t shader_id1, struct CCFeatures *cc_features) {
    /* Extract 4-bit slots: c[cycle][channel][component]
     * shader_id0 layout: cycle 0 color bits [0:15], cycle 0 alpha bits [16:31],
     *                     cycle 1 color bits [32:47], cycle 1 alpha bits [48:63] */
    for (int i = 0; i < 2; i++)         /* cycle */
        for (int j = 0; j < 2; j++)     /* 0=color, 1=alpha */
            for (int k = 0; k < 4; k++) /* component A,B,C,D */
                cc_features->c[i][j][k] = (shader_id0 >> (i * 32 + j * 16 + k * 4)) & 0xf;

    /* Option flags from separate word */
    cc_features->opt_alpha = (shader_id1 & SHADER_OPT_ALPHA) != 0;
    cc_features->opt_fog = (shader_id1 & SHADER_OPT_FOG) != 0;
    cc_features->opt_texture_edge = (shader_id1 & SHADER_OPT_TEXTURE_EDGE) != 0;
    cc_features->opt_noise = (shader_id1 & SHADER_OPT_NOISE) != 0;
    cc_features->opt_2cyc = (shader_id1 & SHADER_OPT_2CYC) != 0;

    cc_features->clamp[0][0] = (shader_id1 & SHADER_OPT_TEXEL0_CLAMP_S) != 0;
    cc_features->clamp[0][1] = (shader_id1 & SHADER_OPT_TEXEL0_CLAMP_T) != 0;
    cc_features->clamp[1][0] = (shader_id1 & SHADER_OPT_TEXEL1_CLAMP_S) != 0;
    cc_features->clamp[1][1] = (shader_id1 & SHADER_OPT_TEXEL1_CLAMP_T) != 0;
    cc_features->tile_mask[0][0] = (shader_id1 & SHADER_OPT_TEXEL0_MASK_S) != 0;
    cc_features->tile_mask[0][1] = (shader_id1 & SHADER_OPT_TEXEL0_MASK_T) != 0;
    cc_features->tile_mask[1][0] = (shader_id1 & SHADER_OPT_TEXEL1_MASK_S) != 0;
    cc_features->tile_mask[1][1] = (shader_id1 & SHADER_OPT_TEXEL1_MASK_T) != 0;
    cc_features->n64_filter[0] = (shader_id1 & SHADER_OPT_TEXEL0_N64_FILTER) != 0;
    cc_features->n64_filter[1] = (shader_id1 & SHADER_OPT_TEXEL1_N64_FILTER) != 0;
    cc_features->noperspective_texcoords = (shader_id1 & SHADER_OPT_NOPERSPECTIVE_TEXCOORDS) != 0;
    cc_features->noperspective_inputs = (shader_id1 & SHADER_OPT_NOPERSPECTIVE_INPUTS) != 0;
    cc_features->noperspective_fog = (shader_id1 & SHADER_OPT_NOPERSPECTIVE_FOG) != 0;
    cc_features->diag_color_scale = (shader_id1 & SHADER_OPT_DIAG_COLOR_SCALE) != 0;
    cc_features->n64_filter_always_3point = (shader_id1 & SHADER_OPT_N64_FILTER_ALWAYS_3POINT) != 0;
    cc_features->diag_alpha_from_tex_intensity = (shader_id1 & SHADER_OPT_DIAG_ALPHA_FROM_TEX_INTENSITY) != 0;
    cc_features->diag_xlu_coverage_wrap_thin = (shader_id1 & SHADER_OPT_DIAG_XLU_COVERAGE_WRAP_THIN) != 0;
    cc_features->diag_rdp_memory_blend = (shader_id1 & SHADER_OPT_DIAG_RDP_MEMORY_BLEND) != 0;
    cc_features->diag_rdp_cvg_memory_blend = (shader_id1 & SHADER_OPT_DIAG_RDP_CVG_MEMORY_BLEND) != 0;
    cc_features->diag_alpha_scale = (shader_id1 & SHADER_OPT_DIAG_ALPHA_SCALE) != 0;
    cc_features->room_water_alpha_suppress = (shader_id1 & SHADER_OPT_ROOM_WATER_ALPHA_SUPPRESS) != 0;
    cc_features->opt_world_pos = (shader_id1 & SHADER_OPT_WORLD_POS) != 0;
    cc_features->opt_sun_shadow = (shader_id1 & SHADER_OPT_SUN_SHADOW) != 0;
    cc_features->opt_dfdx_light = (shader_id1 & SHADER_OPT_DFDX_LIGHT) != 0;

    cc_features->used_textures[0] = false;
    cc_features->used_textures[1] = false;
    cc_features->num_inputs = 0;

    for (int i = 0; i < 2; i++) {       /* cycle */
        for (int j = 0; j < 2; j++) {   /* channel */
            for (int k = 0; k < 4; k++) {
                uint8_t v = cc_features->c[i][j][k];
                if (v >= SHADER_INPUT_1 && v <= SHADER_INPUT_7) {
                    if (v > cc_features->num_inputs)
                        cc_features->num_inputs = v;
                }
                if (v == SHADER_TEXEL0 || v == SHADER_TEXEL0A)
                    cc_features->used_textures[0] = true;
                if (v == SHADER_TEXEL1 || v == SHADER_TEXEL1A)
                    cc_features->used_textures[1] = true;
            }
        }

        /* Per-cycle derived booleans */
        for (int j = 0; j < 2; j++) {
            cc_features->do_single[i][j]   = cc_features->c[i][j][2] == SHADER_0;
            cc_features->do_multiply[i][j] = cc_features->c[i][j][1] == SHADER_0 &&
                                              cc_features->c[i][j][3] == SHADER_0;
            cc_features->do_mix[i][j]      = cc_features->c[i][j][1] == cc_features->c[i][j][3];
        }
        cc_features->color_alpha_same[i] =
            ((shader_id0 >> (i * 32)) & 0xFFFF) == ((shader_id0 >> (i * 32 + 16)) & 0xFFFF);
    }
}
