/**
 * gfx_opengl.c — OpenGL 3.3 rendering backend with GLSL shader generation.
 *
 * Adapted from Emill/n64-fast3d-engine (n64-fast3d-engine license — modified
 * BSD-2-Clause; see src/platform/fast3d/PROVENANCE.md).
 * Changes: uses glad instead of GLEW/SDL2_opengles2, removed ENABLE_OPENGL guard.
 */
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <platform_stdio.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#ifdef MGB64_PORTMASTER_GLES
#include <GLES3/gl32.h>
#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <glad/glad.h>
#endif
#include <SDL.h>

#include "gfx_cc.h"
#include "gfx_mipgen.h"        /* mdkr64: g_pcMipmaps, g_gfxSamplerLod0Only */
#include "gfx_texture_edge.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"
#include "gfx_shadow_cascade.h"
#include "gfx_shadow_frame.h"
#include "gfx_pc_dkr.h"        /* gfx_dkr_replay_pass_active — noise seed guard */
#include "../gfx_pc.h"
#include "front.h"
#include "othermodemicrocode.h"
#include "vi.h"

/* Verbose diagnostic flag from gfx_pc.c */
extern int g_diag_verbose;
/* Render/post-FX uniform state shared with the other backend (see header). */
#include "gfx_uniforms.h"

#define PC_RETRO_FILTER_AUTO 0
#define PC_RETRO_FILTER_OFF  1
#define PC_RETRO_FILTER_ON   2

/* GL_DEPTH_CLAMP support — defined in gfx_pc.c, set once here in
 * gfx_opengl_init() before any rendering, read by CPU clipper and
 * shader generation. Never changes after init. */
extern bool g_depth_clamp_enabled;

#ifndef GL_DEPTH_CLAMP
#define GL_DEPTH_CLAMP 0x864F
#endif

/* Anisotropic filtering extension (part of GL 4.6 core, extension on earlier) */
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

struct ShaderProgram {
    uint64_t shader_id0;
    uint32_t shader_id1;
    GLuint opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    GLint attrib_locations[32];
    uint8_t attrib_sizes[32];
    uint8_t num_attribs;
    bool used_noise;
    GLint frame_count_location;
    GLint window_height_location;
    bool used_n64_filter;
    GLint n64_filter_scale_location;
    bool diag_rdp_memory_blend;
    bool diag_rdp_cvg_memory_blend;
    GLint diag_framebuffer_origin_location;
    GLint diag_viewport_location;
    /* RL-5 smooth-normal, linear-light directional sun uniforms. */
    bool opt_dfdx_light;
    GLint sun_color_location;
    GLint sun_strength_location;
    /* Texture-cutout (alpha-edge) shader: drives GL_SAMPLE_ALPHA_TO_COVERAGE
     * when the multisample scene target is bound (see gfx_opengl_update_a2c_state). */
    bool opt_texture_edge;
    /* W1.E3.T4: sun-shadow receiver uniform locations (opt_sun_shadow). */
    bool opt_sun_shadow;
    GLint shadow_mat_location;
    GLint shadow_params_location;
    GLint shadow_splits_layers_location;
};

/* Growable, pointer-stable pool of heap-allocated shader programs. Entries are
 * never evicted/reused — `gfx_pc.c` caches raw `prg` pointers indefinitely
 * (combiners, comb->prg) and trusts pointer identity for the lifetime of the
 * process, so a wrap-and-reuse scheme (the old fixed 256-slot array) would
 * corrupt live combiners once a 257th distinct shader variant was compiled.
 * Mirrors the Metal backend's growable `s_shaders` array (gfx_metal.mm). */
static struct ShaderProgram **shader_program_pool;
static int shader_program_pool_size;
static int shader_program_pool_cap;

/* T10 telemetry: report the session's final shader-variant count (the
 * pre-dbd3c06 fixed pool corrupted state past 256 variants). */
static void gfx_opengl_log_shader_pool_size_at_exit(void) {
    fprintf(stderr, "[fast3d] GL shader pool final size: %d variant(s)%s\n",
            shader_program_pool_size,
            shader_program_pool_size > 256 ? " (EXCEEDS old fixed pool of 256)" : "");
}

/* W1.E3 (T2/T4): sun shadow depth-only pass + receiver state (capture-and-replay,
 * §4.5). Declared here (before gfx_opengl_set_uniforms) so the receiver upload can
 * read g_shadow_depth_tex / g_shadow_tex_res. */
static GLuint g_shadow_fbo;
static GLuint g_shadow_depth_tex;  /* GL_DEPTH_COMPONENT24 array, compare-mode */
static int    g_shadow_tex_res;    /* current allocation resolution */
static int    g_shadow_tex_layers;
static GLuint g_shadow_program;    /* depth-only geometry shader */
static GLint  g_shadow_mat_loc;    /* uShadowMat uniform */
static GLuint g_shadow_vbo;        /* dynamic caster-geometry upload */
static GLuint g_shadow_vao;
static GfxShadowPlan g_shadow_plan;
static int g_shadow_receiver_view = -1;
static GLuint opengl_vbo;
static GLuint opengl_vao;
static struct ShaderProgram *current_shader_program;

static uint32_t frame_count;
static uint32_t current_height;
static int g_diag_noperspective_inputs = -1; /* GE007_DIAG_NOPERSPECTIVE_INPUTS=1 */
static int g_diag_noperspective_texcoords = -1; /* GE007_DIAG_NOPERSPECTIVE_TEXCOORDS=1 */
static int g_diag_quantize_combiner = -1; /* GE007_DIAG_QUANTIZE_COMBINER=1 */
static int g_diag_settex_cc_color_scale_checked; /* GE007_DIAG_SETTEX_CC_COLOR_SCALE_VALUE=N */
static float g_diag_settex_cc_color_scale_value = 1.02f;
static int g_diag_settex_cc_alpha_scale_checked; /* GE007_DIAG_SETTEX_CC_ALPHA_SCALE_VALUE=N */
static float g_diag_settex_cc_alpha_scale_value = 1.0f;
static int g_diag_n64_filter_always_3point = -1; /* GE007_DIAG_N64_FILTER_ALWAYS_3POINT=1 */
static int g_diag_n64_filter_nearest_threshold_checked; /* GE007_DIAG_N64_FILTER_NEAREST_THRESHOLD=N */
static int g_diag_n64_filter_nearest_threshold_enabled;
static float g_diag_n64_filter_nearest_threshold = 1.0f;
static int g_diag_n64_filter_clamped_non_texedge_nearest_threshold_checked; /* GE007_DIAG_N64_FILTER_CLAMPED_NON_TEXEDGE_NEAREST_THRESHOLD=N */
static int g_diag_n64_filter_clamped_non_texedge_nearest_threshold_enabled;
static float g_diag_n64_filter_clamped_non_texedge_nearest_threshold = 1.0f;
static int g_diag_n64_filter_non_texedge_nearest_threshold_checked; /* GE007_DIAG_N64_FILTER_NON_TEXEDGE_NEAREST_THRESHOLD=N */
static int g_diag_n64_filter_non_texedge_nearest_threshold_enabled;
static float g_diag_n64_filter_non_texedge_nearest_threshold = 1.0f;
static int g_diag_zmode_xlu_less = -1; /* GE007_DIAG_ZMODE_XLU_LESS=1 */
static int g_diag_zmode_dec_less = -1; /* GE007_DIAG_ZMODE_DEC_LESS=1 */
static int g_diag_zmode_dec_no_poly_offset = -1; /* GE007_DIAG_ZMODE_DEC_NO_POLY_OFFSET=1 */
static int g_diag_zmode_dec_offset_checked; /* GE007_DIAG_ZMODE_DEC_OFFSET_FACTOR/UNITS=N */
static float g_diag_zmode_dec_offset_factor = -2.0f;
static float g_diag_zmode_dec_offset_units = -2.0f;
static int g_diag_alpha_blend_checked; /* GE007_DIAG_ALPHA_BLEND=premult|add|copy|inv_alpha */
static int g_diag_alpha_blend_mode;
static int g_diag_alpha_coverage_logged;
static int g_diag_xlu_coverage_wrap_thin_rate_checked; /* GE007_DIAG_XLU_COVERAGE_WRAP_THIN_RATE=N */
static float g_diag_xlu_coverage_wrap_thin_rate = 0.25f;
static int g_diag_xlu_coverage_stencil_checked; /* GE007_DIAG_XLU_COVERAGE_STENCIL_CC=... */
static int g_diag_xlu_coverage_stencil_enabled;
static int g_diag_xlu_coverage_stencil_increment_checked; /* GE007_DIAG_XLU_COVERAGE_STENCIL_INCREMENT=N */
static int g_diag_xlu_coverage_stencil_increment = 4;
static int g_diag_xlu_coverage_stencil_logged;
static int g_diag_xlu_rdp_memory_blend_checked; /* GE007_DIAG_XLU_RDP_MEMORY_BLEND_CC=... */
static int g_diag_xlu_rdp_memory_blend_enabled;
static int g_diag_xlu_rdp_memory_blend_logged;
static int g_diag_xlu_rdp_cvg_memory_blend_checked; /* GE007_DIAG_XLU_RDP_CVG_MEMORY_BLEND_CC=... */
static int g_diag_xlu_rdp_cvg_memory_blend_enabled;
static int g_diag_xlu_rdp_cvg_memory_blend_logged;
static int g_room_xlu_cvg_memory_checked; /* GE007_ROOM_XLU_CVG_MEMORY / GE007_DISABLE_ROOM_XLU_CVG_MEMORY */
static int g_room_xlu_cvg_memory_enabled;
static int g_diag_alpha_from_tex_intensity_mix_checked; /* GE007_DIAG_ALPHA_FROM_TEX_INTENSITY_MIX=N */
static float g_diag_alpha_from_tex_intensity_mix = 1.0f;

static bool gfx_diag_noperspective_inputs_enabled(void) {
    if (g_diag_noperspective_inputs < 0) {
        g_diag_noperspective_inputs =
            (getenv("GE007_DIAG_NOPERSPECTIVE_INPUTS") != NULL) ? 1 : 0;
        if (g_diag_noperspective_inputs) {
            fprintf(stderr,
                    "[fast3d] DIAG NOPERSPECTIVE SHADER INPUTS ENABLED "
                    "(GE007_DIAG_NOPERSPECTIVE_INPUTS)\n");
            fflush(stderr);
        }
    }
    return g_diag_noperspective_inputs > 0;
}

static bool gfx_diag_n64_filter_always_3point_enabled(void) {
    if (g_diag_n64_filter_always_3point < 0) {
        g_diag_n64_filter_always_3point =
            (getenv("GE007_DIAG_N64_FILTER_ALWAYS_3POINT") != NULL) ? 1 : 0;
        if (g_diag_n64_filter_always_3point) {
            fprintf(stderr,
                    "[fast3d] DIAG N64 FILTER ALWAYS 3POINT ENABLED "
                    "(GE007_DIAG_N64_FILTER_ALWAYS_3POINT)\n");
            fflush(stderr);
        }
    }
    return g_diag_n64_filter_always_3point > 0;
}

static float gfx_parse_diag_float_threshold(const char *env, float fallback) {
    float value = fallback;
    if (env != NULL && env[0] != '\0') {
        char *end = NULL;
        value = strtof(env, &end);
        if (end == env || value != value) {
            value = fallback;
        }
    }
    if (value < 0.0f) value = 0.0f;
    if (value > 4.0f) value = 4.0f;
    return value;
}

static float gfx_parse_diag_unit_float(const char *env, float fallback) {
    float value = fallback;
    if (env != NULL && env[0] != '\0') {
        char *end = NULL;
        value = strtof(env, &end);
        if (end == env || value != value) {
            value = fallback;
        }
    }
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    return value;
}

static float gfx_diag_alpha_from_tex_intensity_mix(void) {
    if (!g_diag_alpha_from_tex_intensity_mix_checked) {
        const char *env = getenv("GE007_DIAG_ALPHA_FROM_TEX_INTENSITY_MIX");
        g_diag_alpha_from_tex_intensity_mix =
            gfx_parse_diag_unit_float(env, g_diag_alpha_from_tex_intensity_mix);
        if (env != NULL && env[0] != '\0') {
            fprintf(stderr,
                    "[fast3d] DIAG ALPHA FROM TEX INTENSITY mix=%.6f "
                    "(GE007_DIAG_ALPHA_FROM_TEX_INTENSITY_MIX)\n",
                    g_diag_alpha_from_tex_intensity_mix);
            fflush(stderr);
        }
        g_diag_alpha_from_tex_intensity_mix_checked = 1;
    }
    return g_diag_alpha_from_tex_intensity_mix;
}

static float gfx_diag_xlu_coverage_wrap_thin_rate(void) {
    if (!g_diag_xlu_coverage_wrap_thin_rate_checked) {
        const char *env = getenv("GE007_DIAG_XLU_COVERAGE_WRAP_THIN_RATE");
        g_diag_xlu_coverage_wrap_thin_rate =
            gfx_parse_diag_unit_float(env, g_diag_xlu_coverage_wrap_thin_rate);
        fprintf(stderr,
                "[fast3d] DIAG XLU COVERAGE WRAP THIN rate=%.6f "
                "(GE007_DIAG_XLU_COVERAGE_WRAP_THIN_RATE)\n",
                g_diag_xlu_coverage_wrap_thin_rate);
        fflush(stderr);
        g_diag_xlu_coverage_wrap_thin_rate_checked = 1;
    }
    return g_diag_xlu_coverage_wrap_thin_rate;
}

static bool gfx_diag_xlu_coverage_stencil_enabled(void) {
    if (!g_diag_xlu_coverage_stencil_checked) {
        const char *env = getenv("GE007_DIAG_XLU_COVERAGE_STENCIL_CC");
        g_diag_xlu_coverage_stencil_enabled =
            (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        g_diag_xlu_coverage_stencil_checked = 1;
    }
    return g_diag_xlu_coverage_stencil_enabled != 0;
}

static int gfx_diag_xlu_coverage_stencil_increment(void) {
    if (!g_diag_xlu_coverage_stencil_increment_checked) {
        const char *env = getenv("GE007_DIAG_XLU_COVERAGE_STENCIL_INCREMENT");
        if (env != NULL && env[0] != '\0') {
            int value = atoi(env);
            if (value < 1) value = 1;
            if (value > 8) value = 8;
            g_diag_xlu_coverage_stencil_increment = value;
        }
        fprintf(stderr,
                "[fast3d] DIAG XLU COVERAGE STENCIL increment=%d "
                "(GE007_DIAG_XLU_COVERAGE_STENCIL_INCREMENT)\n",
                g_diag_xlu_coverage_stencil_increment);
        fflush(stderr);
        g_diag_xlu_coverage_stencil_increment_checked = 1;
    }
    return g_diag_xlu_coverage_stencil_increment;
}

static bool gfx_diag_xlu_rdp_memory_blend_enabled(void) {
    if (!g_diag_xlu_rdp_memory_blend_checked) {
        const char *env = getenv("GE007_DIAG_XLU_RDP_MEMORY_BLEND_CC");
        g_diag_xlu_rdp_memory_blend_enabled =
            (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        g_diag_xlu_rdp_memory_blend_checked = 1;
    }
    return g_diag_xlu_rdp_memory_blend_enabled != 0;
}

static bool gfx_diag_xlu_rdp_cvg_memory_blend_enabled(void) {
    if (!g_diag_xlu_rdp_cvg_memory_blend_checked) {
        const char *env = getenv("GE007_DIAG_XLU_RDP_CVG_MEMORY_BLEND_CC");
        g_diag_xlu_rdp_cvg_memory_blend_enabled =
            (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        g_diag_xlu_rdp_cvg_memory_blend_checked = 1;
    }
    return g_diag_xlu_rdp_cvg_memory_blend_enabled != 0;
}

static bool gfx_opengl_room_xlu_cvg_memory_enabled(void) {
    if (!g_room_xlu_cvg_memory_checked) {
        const char *disable_env = getenv("GE007_DISABLE_ROOM_XLU_CVG_MEMORY");
        const char *enable_env = getenv("GE007_ROOM_XLU_CVG_MEMORY");

        g_room_xlu_cvg_memory_enabled = 1;
        if ((disable_env != NULL && disable_env[0] != '\0' && disable_env[0] != '0') ||
            (enable_env != NULL && enable_env[0] == '0')) {
            g_room_xlu_cvg_memory_enabled = 0;
        }
        g_room_xlu_cvg_memory_checked = 1;
    }
    return g_room_xlu_cvg_memory_enabled != 0;
}

static float gfx_diag_n64_filter_nearest_threshold(bool texture_edge,
                                                   bool clamped,
                                                   float default_threshold)
{
    if (!g_diag_n64_filter_nearest_threshold_checked) {
        const char *env = getenv("GE007_DIAG_N64_FILTER_NEAREST_THRESHOLD");
        if (env != NULL && env[0] != '\0') {
            g_diag_n64_filter_nearest_threshold =
                gfx_parse_diag_float_threshold(env, default_threshold);
            g_diag_n64_filter_nearest_threshold_enabled = 1;
            fprintf(stderr,
                    "[fast3d] DIAG N64 FILTER NEAREST THRESHOLD value=%.6f "
                    "(GE007_DIAG_N64_FILTER_NEAREST_THRESHOLD)\n",
                    g_diag_n64_filter_nearest_threshold);
            fflush(stderr);
        }
        g_diag_n64_filter_nearest_threshold_checked = 1;
    }
    if (g_diag_n64_filter_nearest_threshold_enabled) {
        return g_diag_n64_filter_nearest_threshold;
    }

    if (!g_diag_n64_filter_clamped_non_texedge_nearest_threshold_checked) {
        const char *env = getenv("GE007_DIAG_N64_FILTER_CLAMPED_NON_TEXEDGE_NEAREST_THRESHOLD");
        if (env != NULL && env[0] != '\0') {
            g_diag_n64_filter_clamped_non_texedge_nearest_threshold =
                gfx_parse_diag_float_threshold(env,
                                               default_threshold);
            g_diag_n64_filter_clamped_non_texedge_nearest_threshold_enabled = 1;
            fprintf(stderr,
                    "[fast3d] DIAG N64 FILTER CLAMPED NON-TEXEDGE NEAREST THRESHOLD value=%.6f "
                    "(GE007_DIAG_N64_FILTER_CLAMPED_NON_TEXEDGE_NEAREST_THRESHOLD)\n",
                    g_diag_n64_filter_clamped_non_texedge_nearest_threshold);
            fflush(stderr);
        }
        g_diag_n64_filter_clamped_non_texedge_nearest_threshold_checked = 1;
    }
    if (clamped && !texture_edge &&
        g_diag_n64_filter_clamped_non_texedge_nearest_threshold_enabled) {
        return g_diag_n64_filter_clamped_non_texedge_nearest_threshold;
    }

    if (!g_diag_n64_filter_non_texedge_nearest_threshold_checked) {
        const char *env = getenv("GE007_DIAG_N64_FILTER_NON_TEXEDGE_NEAREST_THRESHOLD");
        if (env != NULL && env[0] != '\0') {
            g_diag_n64_filter_non_texedge_nearest_threshold =
                gfx_parse_diag_float_threshold(env, default_threshold);
            g_diag_n64_filter_non_texedge_nearest_threshold_enabled = 1;
            fprintf(stderr,
                    "[fast3d] DIAG N64 FILTER NON-TEXEDGE NEAREST THRESHOLD value=%.6f "
                    "(GE007_DIAG_N64_FILTER_NON_TEXEDGE_NEAREST_THRESHOLD)\n",
                    g_diag_n64_filter_non_texedge_nearest_threshold);
            fflush(stderr);
        }
        g_diag_n64_filter_non_texedge_nearest_threshold_checked = 1;
    }
    if (!texture_edge && g_diag_n64_filter_non_texedge_nearest_threshold_enabled) {
        return g_diag_n64_filter_non_texedge_nearest_threshold;
    }

    return default_threshold;
}

static bool gfx_diag_noperspective_texcoords_enabled(void) {
    if (g_diag_noperspective_texcoords < 0) {
        g_diag_noperspective_texcoords =
            (getenv("GE007_DIAG_NOPERSPECTIVE_TEXCOORDS") != NULL) ? 1 : 0;
        if (g_diag_noperspective_texcoords) {
            fprintf(stderr,
                    "[fast3d] DIAG NOPERSPECTIVE TEXCOORDS ENABLED "
                    "(GE007_DIAG_NOPERSPECTIVE_TEXCOORDS)\n");
            fflush(stderr);
        }
    }
    return g_diag_noperspective_texcoords > 0;
}

static bool gfx_diag_quantize_combiner_enabled(void) {
    if (g_diag_quantize_combiner < 0) {
        g_diag_quantize_combiner =
            (getenv("GE007_DIAG_QUANTIZE_COMBINER") != NULL) ? 1 : 0;
        if (g_diag_quantize_combiner) {
            fprintf(stderr,
                    "[fast3d] DIAG QUANTIZE COMBINER ENABLED "
                    "(GE007_DIAG_QUANTIZE_COMBINER)\n");
            fflush(stderr);
        }
    }
    return g_diag_quantize_combiner > 0;
}

static float gfx_diag_settex_cc_color_scale_value(void) {
    if (!g_diag_settex_cc_color_scale_checked) {
        const char *env = getenv("GE007_DIAG_SETTEX_CC_COLOR_SCALE_VALUE");
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            float value = strtof(env, &end);
            if (end != env && value == value) {
                if (value < 0.0f) value = 0.0f;
                if (value > 4.0f) value = 4.0f;
                g_diag_settex_cc_color_scale_value = value;
            }
        }
        fprintf(stderr,
                "[fast3d] DIAG SETTEX CC COLOR SCALE value=%.6f "
                "(GE007_DIAG_SETTEX_CC_COLOR_SCALE_VALUE)\n",
                g_diag_settex_cc_color_scale_value);
        fflush(stderr);
        g_diag_settex_cc_color_scale_checked = 1;
    }
    return g_diag_settex_cc_color_scale_value;
}

static float gfx_diag_settex_cc_alpha_scale_value(void) {
    if (!g_diag_settex_cc_alpha_scale_checked) {
        const char *env = getenv("GE007_DIAG_SETTEX_CC_ALPHA_SCALE_VALUE");
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            float value = strtof(env, &end);
            if (end != env && value == value) {
                if (value < 0.0f) value = 0.0f;
                if (value > 4.0f) value = 4.0f;
                g_diag_settex_cc_alpha_scale_value = value;
            }
        }
        fprintf(stderr,
                "[fast3d] DIAG SETTEX CC ALPHA SCALE value=%.6f "
                "(GE007_DIAG_SETTEX_CC_ALPHA_SCALE_VALUE)\n",
                g_diag_settex_cc_alpha_scale_value);
        fflush(stderr);
        g_diag_settex_cc_alpha_scale_checked = 1;
    }
    return g_diag_settex_cc_alpha_scale_value;
}

static bool gfx_diag_zmode_xlu_less_enabled(void) {
    if (g_diag_zmode_xlu_less < 0) {
        g_diag_zmode_xlu_less =
            (getenv("GE007_DIAG_ZMODE_XLU_LESS") != NULL) ? 1 : 0;
        if (g_diag_zmode_xlu_less) {
            fprintf(stderr,
                    "[fast3d] DIAG ZMODE_XLU depth func GL_LESS "
                    "(GE007_DIAG_ZMODE_XLU_LESS)\n");
            fflush(stderr);
        }
    }
    return g_diag_zmode_xlu_less > 0;
}

static bool gfx_diag_zmode_dec_less_enabled(void) {
    if (g_diag_zmode_dec_less < 0) {
        g_diag_zmode_dec_less =
            (getenv("GE007_DIAG_ZMODE_DEC_LESS") != NULL) ? 1 : 0;
        if (g_diag_zmode_dec_less) {
            fprintf(stderr,
                    "[fast3d] DIAG ZMODE_DEC depth func GL_LESS "
                    "(GE007_DIAG_ZMODE_DEC_LESS)\n");
            fflush(stderr);
        }
    }
    return g_diag_zmode_dec_less > 0;
}

static bool gfx_diag_zmode_dec_no_poly_offset_enabled(void) {
    if (g_diag_zmode_dec_no_poly_offset < 0) {
        g_diag_zmode_dec_no_poly_offset =
            (getenv("GE007_DIAG_ZMODE_DEC_NO_POLY_OFFSET") != NULL) ? 1 : 0;
        if (g_diag_zmode_dec_no_poly_offset) {
            fprintf(stderr,
                    "[fast3d] DIAG ZMODE_DEC polygon offset disabled "
                    "(GE007_DIAG_ZMODE_DEC_NO_POLY_OFFSET)\n");
            fflush(stderr);
        }
    }
    return g_diag_zmode_dec_no_poly_offset > 0;
}

static float gfx_parse_diag_depth_offset(const char *env, float fallback) {
    float value = fallback;
    if (env != NULL && env[0] != '\0') {
        char *end = NULL;
        value = strtof(env, &end);
        if (end == env || value != value) {
            value = fallback;
        }
    }
    if (value < -16.0f) value = -16.0f;
    if (value > 16.0f) value = 16.0f;
    return value;
}

static void gfx_diag_zmode_dec_offset_values(float *factor, float *units) {
    if (!g_diag_zmode_dec_offset_checked) {
        const char *factor_env = getenv("GE007_DIAG_ZMODE_DEC_OFFSET_FACTOR");
        const char *units_env = getenv("GE007_DIAG_ZMODE_DEC_OFFSET_UNITS");
        if (factor_env != NULL && factor_env[0] != '\0') {
            g_diag_zmode_dec_offset_factor =
                gfx_parse_diag_depth_offset(factor_env,
                                            g_diag_zmode_dec_offset_factor);
        }
        if (units_env != NULL && units_env[0] != '\0') {
            g_diag_zmode_dec_offset_units =
                gfx_parse_diag_depth_offset(units_env,
                                            g_diag_zmode_dec_offset_units);
        }
        if ((factor_env != NULL && factor_env[0] != '\0') ||
            (units_env != NULL && units_env[0] != '\0')) {
            fprintf(stderr,
                    "[fast3d] DIAG ZMODE_DEC polygon offset factor=%.6f units=%.6f "
                    "(GE007_DIAG_ZMODE_DEC_OFFSET_FACTOR/UNITS)\n",
                    g_diag_zmode_dec_offset_factor,
                    g_diag_zmode_dec_offset_units);
            fflush(stderr);
        }
        g_diag_zmode_dec_offset_checked = 1;
    }
    *factor = g_diag_zmode_dec_offset_factor;
    *units = g_diag_zmode_dec_offset_units;
}

static int gfx_diag_alpha_blend_mode(void) {
    if (!g_diag_alpha_blend_checked) {
        const char *env = getenv("GE007_DIAG_ALPHA_BLEND");

        if (env != NULL && env[0] != '\0') {
            if (strcmp(env, "premult") == 0) {
                g_diag_alpha_blend_mode = 1;
            } else if (strcmp(env, "add") == 0) {
                g_diag_alpha_blend_mode = 2;
            } else if (strcmp(env, "copy") == 0) {
                g_diag_alpha_blend_mode = 3;
            } else if (strcmp(env, "inv_alpha") == 0) {
                g_diag_alpha_blend_mode = 4;
            } else {
                fprintf(stderr,
                        "[fast3d] Ignoring invalid GE007_DIAG_ALPHA_BLEND=%s "
                        "(expected premult, add, copy, or inv_alpha)\n",
                        env);
                fflush(stderr);
            }
        }
        if (g_diag_alpha_blend_mode) {
            fprintf(stderr,
                    "[fast3d] DIAG alpha blend mode=%s "
                    "(GE007_DIAG_ALPHA_BLEND)\n",
                    env);
            fflush(stderr);
        }
        g_diag_alpha_blend_checked = 1;
    }

    return g_diag_alpha_blend_mode;
}

static bool gfx_opengl_z_is_from_0_to_1(void) {
    return false;
}

static void gfx_opengl_vertex_array_set_attribs(struct ShaderProgram *prg) {
    size_t num_floats = prg->num_floats;
    size_t pos = 0;

    for (int i = 0; i < prg->num_attribs; i++) {
        /* A declared-but-dead attribute (e.g. aWorldPos with no live consumer under
         * GE007_FORCE_WORLD_ATTRS + diag off) can link to location -1. Skip the GL
         * calls — glEnableVertexAttribArray(-1) would raise GL_INVALID_VALUE — but
         * still advance the stride offset so later attributes stay aligned. */
        if (prg->attrib_locations[i] >= 0) {
            glEnableVertexAttribArray(prg->attrib_locations[i]);
            glVertexAttribPointer(prg->attrib_locations[i], prg->attrib_sizes[i], GL_FLOAT, GL_FALSE, num_floats * sizeof(float), (void *)(pos * sizeof(float)));
        }
        pos += prg->attrib_sizes[i];
    }
}

static float gfx_opengl_axis_filter_scale(uint32_t drawable_size,
                                          int logical_size,
                                          int fallback_logical_size)
{
    float scale;

    if (logical_size <= 0) {
        logical_size = fallback_logical_size;
    }
    if (drawable_size == 0 || logical_size <= 0) {
        return 1.0f;
    }

    scale = (float)drawable_size / (float)logical_size;
    if (scale < 1.0f) {
        return 1.0f;
    }
    if (scale > 64.0f) {
        return 64.0f;
    }
    return scale;
}

static void gfx_opengl_set_uniforms(struct ShaderProgram *prg) {
    if (prg->used_noise) {
        glUniform1i(prg->frame_count_location, frame_count);
        glUniform1i(prg->window_height_location, current_height);
    }
    if (prg->used_n64_filter && prg->n64_filter_scale_location >= 0) {
        /* dFdx/dFdy are measured in native drawable pixels. Scale them back
         * to the VI/logical pixel grid before applying N64 filter thresholds. */
        float scale_x =
            gfx_opengl_axis_filter_scale(gfx_current_dimensions.width,
                                          viGetX(),
                                          DESIRED_SCREEN_WIDTH);
        float scale_y =
            gfx_opengl_axis_filter_scale(gfx_current_dimensions.height,
                                          viGetY(),
                                          DESIRED_SCREEN_HEIGHT);
        glUniform2f(prg->n64_filter_scale_location, scale_x, scale_y);
    }
    /* W1.E3.T4: sun-shadow receiver uniforms + shadow map on unit 5. The matrix is
     * m[row][col] (column-vector M*v); GL wants column-major -> upload transposed. */
    if (prg->opt_sun_shadow && prg->shadow_mat_location >= 0) {
        float matrices[GFX_SHADOW_MAX_CASCADES][4][4] = {{{0}}};
        float split_start = 0.0f;
        float split_end = 0.0f;
        float layer0 = 0.0f;
        float layer1 = 0.0f;
        float bias_ndc = 0.002f;
        int cascade_count = 0;
        if (g_shadow_plan.valid &&
            g_shadow_receiver_view >= 0 &&
            g_shadow_receiver_view < (int)g_shadow_plan.view_count) {
            cascade_count =
                (int)g_shadow_plan.budget.cascades_per_view;
            size_t base =
                (size_t)g_shadow_receiver_view *
                g_shadow_plan.budget.cascades_per_view;
            for (int cascade = 0; cascade < cascade_count; cascade++) {
                memcpy(
                    matrices[cascade],
                    g_shadow_plan.cascades[base + (size_t)cascade]
                        .world_to_clip,
                    sizeof(matrices[cascade]));
            }
            if (cascade_count == 1) {
                memcpy(matrices[1], matrices[0], sizeof(matrices[1]));
            }
            layer0 =
                (float)g_shadow_plan.cascades[base].map_index;
            layer1 = cascade_count > 1
                ? (float)g_shadow_plan.cascades[base + 1].map_index
                : layer0;
            if (cascade_count > 1) {
                split_start =
                    g_shadow_plan.cascades[base + 1].split_near;
                split_end =
                    g_shadow_plan.cascades[base].split_far;
            }
            /*
             * The comparison bias is authored in WORLD units
             * (g_pcSunShadowBias) and normalized here by the planned light
             * z-span: the shader compares normalized depths, so a constant
             * NDC bias would silently scale with however much caster depth
             * this stage spans (it grew several-fold when planning learned
             * to cover the whole static cache) and would differ per track.
             */
            {
                float span = 0.0f;
                for (int cascade = 0; cascade < cascade_count; cascade++) {
                    const GfxShadowCascade *c =
                        &g_shadow_plan.cascades[base + (size_t)cascade];
                    float s = c->light_bounds_max[2] -
                              c->light_bounds_min[2];
                    if (s > span) {
                        span = s;
                    }
                }
                if (span > 1.0f) {
                    bias_ndc = g_pcSunShadowBias / span;
                    if (bias_ndc < 0.0002f) bias_ndc = 0.0002f;
                    if (bias_ndc > 0.01f) bias_ndc = 0.01f;
                }
            }
        }
        glUniformMatrix4fv(
            prg->shadow_mat_location, GFX_SHADOW_MAX_CASCADES,
            GL_TRUE, &matrices[0][0][0]);
        glUniform4f(
            prg->shadow_params_location,
            g_shadow_tex_res > 0
                ? 1.0f / (float)g_shadow_tex_res
                : 1.0f / 2048.0f,
            bias_ndc,
            g_pcSunShadowUmbra,
            (float)cascade_count);
        glUniform4f(
            prg->shadow_splits_layers_location,
            split_start, split_end, layer0, layer1);
        GLint prev_active = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D_ARRAY, g_shadow_depth_tex);
        glActiveTexture((GLenum)prev_active);
    }
    if (prg->opt_dfdx_light) {
        if (prg->sun_color_location >= 0)
            glUniform3f(prg->sun_color_location,
                        g_pc_sun_color_linear[0],
                        g_pc_sun_color_linear[1],
                        g_pc_sun_color_linear[2]);
        if (prg->sun_strength_location >= 0)
            glUniform1f(prg->sun_strength_location,
                        g_pcSunStrength);
    }
}

static void gfx_opengl_unload_shader(struct ShaderProgram *old_prg) {
    if (old_prg != NULL) {
        for (int i = 0; i < old_prg->num_attribs; i++) {
            if (old_prg->attrib_locations[i] >= 0) {
                glDisableVertexAttribArray(old_prg->attrib_locations[i]);
            }
        }
        if (current_shader_program == old_prg) {
            current_shader_program = NULL;
        }
    }
}

static GLuint g_scene_fbo;
static GLuint g_scene_color_tex;
static GLuint g_scene_depth_tex;   /* sampleable single-sample depth (for SSAO/T1.1) */
static GLuint g_scene_msaa_fbo;
static GLuint g_scene_msaa_color_rb;
static GLuint g_scene_msaa_depth_rb;
static int g_scene_w;
static int g_scene_h;
static bool g_scene_has_stencil;
static int g_scene_msaa_w;
static int g_scene_msaa_h;
static int g_scene_msaa_samples;
static bool g_scene_msaa_has_stencil;
static bool g_scene_target_bound;
static bool g_output_overlay_active;
/* True after a frame rendered scene depth into the sampleable (single-sample)
 * g_scene_depth_tex — the precondition for SSAO to read valid depth. */
static bool g_scene_depth_valid;
/* True only while the multisample scene FBO is bound (set in start_frame). */
static bool g_scene_target_multisampled;
/* Tracks the live blend-enable so default A2C only engages on blend-DISABLED cutouts. */
static bool g_blend_disabled = true;
/* Default-off XLU coverage diagnostic; true only for GFX_BLEND_ALPHA_COVERAGE. */
static bool g_blend_alpha_coverage;
/* Default-off XLU coverage-memory diagnostic; true only for stencil blend mode. */
static bool g_blend_alpha_cvg_wrap_stencil;
/* Default-off RDP memory-color diagnostic; true only for memory blend mode. */
static bool g_blend_alpha_rdp_memory;
/* Default-off RDP coverage + memory-color diagnostic. */
static bool g_blend_alpha_rdp_cvg_memory;

/* GL_SAMPLE_ALPHA_TO_COVERAGE is core GL 3.3; provide the token defensively. */
#ifndef GL_SAMPLE_ALPHA_TO_COVERAGE
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#endif

/* A/B escape hatch: GE007_NO_A2C=1 forces alpha-to-coverage off. */
static int g_a2c_force = -1;
static bool gfx_opengl_a2c_enabled(void) {
    if (g_a2c_force < 0) {
        const char *e = getenv("GE007_NO_A2C");
        g_a2c_force = (e && e[0] && strcmp(e, "0") != 0) ? 0 : 1;
    }
    return g_a2c_force != 0;
}

/* Engage alpha-to-coverage only for the default blend-disabled texture-edge
 * cutout path, or for the default-off XLU coverage diagnostic blend mode. Both
 * require a multisample scene target and the env override to allow A2C.
 * Re-evaluated on both shader and blend changes (load_shader runs before
 * set_blend_mode in gfx_pc, and blend can change without a shader change). On
 * the single-sample output/default FB this is always false, so the output pass
 * is unaffected even though it does not save/restore A2C state. */
static void gfx_opengl_update_a2c_state(void) {
    bool cutout_a2c = g_blend_disabled &&
                      current_shader_program != NULL &&
                      current_shader_program->opt_texture_edge;
    bool on = g_scene_target_multisampled &&
              (cutout_a2c || g_blend_alpha_coverage) &&
              gfx_opengl_a2c_enabled();
    if (on) {
        glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    } else {
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    }
}

static void gfx_opengl_load_shader(struct ShaderProgram *new_prg) {
    glUseProgram(new_prg->opengl_program_id);
    current_shader_program = new_prg;
    gfx_opengl_vertex_array_set_attribs(new_prg);
    gfx_opengl_set_uniforms(new_prg);
    gfx_opengl_update_a2c_state();
}

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
    buf[(*len)++] = '\n';
}

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_INPUT_5:
                return with_alpha || !inputs_have_alpha ? "vInput5" : "vInput5.rgb";
            case SHADER_INPUT_6:
                return with_alpha || !inputs_have_alpha ? "vInput6" : "vInput6.rgb";
            case SHADER_INPUT_7:
                return with_alpha || !inputs_have_alpha ? "vInput7" : "vInput7.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" :
                    (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)" : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a" :
                    (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)" : "vec3(texVal1.a, texVal1.a, texVal1.a)");
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "vec4(random(vec3(floor(gl_FragCoord.xy * (240.0 / float(window_height))), float(frame_count))))" :
                    "vec3(random(vec3(floor(gl_FragCoord.xy * (240.0 / float(window_height))), float(frame_count))))";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_INPUT_1: return "vInput1.a";
            case SHADER_INPUT_2: return "vInput2.a";
            case SHADER_INPUT_3: return "vInput3.a";
            case SHADER_INPUT_4: return "vInput4.a";
            case SHADER_INPUT_5: return "vInput5.a";
            case SHADER_INPUT_6: return "vInput6.a";
            case SHADER_INPUT_7: return "vInput7.a";
            case SHADER_TEXEL0: return "texVal0.a";
            case SHADER_TEXEL0A: return "texVal0.a";
            case SHADER_TEXEL1: return "texVal1.a";
            case SHADER_TEXEL1A: return "texVal1.a";
            case SHADER_1: return "1.0";
            case SHADER_COMBINED: return "texel.a";
            case SHADER_NOISE:
                return "random(vec3(floor(gl_FragCoord.xy * (240.0 / float(window_height))), float(frame_count)))";
        }
    }
    return "0.0";
}

static void append_formula(char *buf, size_t *len, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "mix(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

/* W1.E2.T3 validation latch: GE007_WORLD_POS_DIAG makes the fragment shader
 * visualize the interpolated world position (fract(vWorldPos*0.01)) — a stable,
 * world-anchored pattern that confirms the aWorldPos attribute is plumbed and
 * interpolated correctly, identical GL vs Metal. Run-constant so shaders are
 * generated consistently. */
static int gfx_world_pos_diag_enabled(void) {
    static int v = -1;
    if (v < 0) v = (getenv("GE007_WORLD_POS_DIAG") != NULL);
    return v;
}

static struct ShaderProgram *gfx_opengl_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    struct CCFeatures cc_features;
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    char vs_buf[12288];
    char fs_buf[18000];
    size_t vs_len = 0;
    size_t fs_len = 0;
    size_t num_floats = 4;
    const char *input_interp =
        (gfx_diag_noperspective_inputs_enabled() || cc_features.noperspective_inputs) ?
        "noperspective " : "";
    const char *texcoord_interp =
        (gfx_diag_noperspective_texcoords_enabled() || cc_features.noperspective_texcoords) ?
        "noperspective " : "";
    const char *fog_interp = cc_features.noperspective_fog ? "noperspective " : "";
    bool quantize_combiner = gfx_diag_quantize_combiner_enabled();
    bool uses_tile_mask =
        cc_features.tile_mask[0][0] || cc_features.tile_mask[0][1] ||
        cc_features.tile_mask[1][0] || cc_features.tile_mask[1][1];

    /* Use GLSL 150 for macOS Core Profile, 320 es for GLES3, 330 elsewhere */
#ifdef MGB64_PORTMASTER_GLES
    append_line(vs_buf, &vs_len, "#version 320 es");
    append_line(vs_buf, &vs_len, "precision mediump float;");
#elif defined(__APPLE__)
    append_line(vs_buf, &vs_len, "#version 150");
#else
    append_line(vs_buf, &vs_len, "#version 330 core");
#endif
    append_line(vs_buf, &vs_len, "in vec4 aVtxPos;");
    if (cc_features.opt_alpha && cc_features.diag_rdp_cvg_memory_blend) {
        append_line(vs_buf, &vs_len, "in vec4 aDiagTri01;");
        append_line(vs_buf, &vs_len, "in vec2 aDiagTri2;");
        append_line(vs_buf, &vs_len, "noperspective out vec4 vDiagTri01;");
        append_line(vs_buf, &vs_len, "noperspective out vec2 vDiagTri2;");
        num_floats += 6;
    }
    /* W1.E2.T3: world-space position attribute, at the same ordinal as the pack
     * order (after pos/diag, before texcoords) so the VBO stride stays aligned. */
    if (cc_features.opt_world_pos) {
        append_line(vs_buf, &vs_len, "in vec3 aWorldPos;");
        append_line(vs_buf, &vs_len, "out vec3 vWorldPos;");
        num_floats += 3;
    }
    if (cc_features.opt_sun_shadow) {
        append_line(vs_buf, &vs_len, "out float vShadowDepth;");
    }
    /* RL-5: real smooth model normal and object-local sun direction. */
    if (cc_features.opt_dfdx_light) {
        append_line(vs_buf, &vs_len, "in vec3 aSmoothNormal;");
        append_line(vs_buf, &vs_len, "in vec3 aLightDir;");
        append_line(vs_buf, &vs_len, "out vec3 vSmoothNormal;");
        append_line(vs_buf, &vs_len, "out vec3 vLightDir;");
        num_floats += 6;
    }
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            vs_len += ge007_sprintf(vs_buf + vs_len, "in vec2 aTexCoord%d;\n", i);
            vs_len += ge007_sprintf(vs_buf + vs_len, "%sout vec2 vTexCoord%d;\n",
                                     texcoord_interp, i);
            num_floats += 2;
            for (int axis = 0; axis < 2; axis++) {
                if (cc_features.clamp[i][axis]) {
                    const char axis_name = axis == 0 ? 'S' : 'T';
                    vs_len += ge007_sprintf(vs_buf + vs_len,
                                             "in float aTexClamp%c%d;\n",
                                             axis_name, i);
                    vs_len += ge007_sprintf(vs_buf + vs_len,
                                             "%sout float vTexClamp%c%d;\n",
                                             texcoord_interp, axis_name, i);
                    num_floats += 1;
                }
                if (cc_features.tile_mask[i][axis]) {
                    const char axis_name = axis == 0 ? 'S' : 'T';
                    vs_len += ge007_sprintf(vs_buf + vs_len,
                                             "in float aTexMask%c%d;\n",
                                             axis_name, i);
                    vs_len += ge007_sprintf(vs_buf + vs_len,
                                             "%sout float vTexMask%c%d;\n",
                                             texcoord_interp, axis_name, i);
                    num_floats += 1;
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "in vec4 aFog;");
        vs_len += ge007_sprintf(vs_buf + vs_len, "%sout vec4 vFog;\n",
                                 fog_interp);
        num_floats += 4;
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += ge007_sprintf(vs_buf + vs_len, "in vec%d aInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        vs_len += ge007_sprintf(vs_buf + vs_len, "%sout vec%d vInput%d;\n",
                                 input_interp, cc_features.opt_alpha ? 4 : 3, i + 1);
        num_floats += cc_features.opt_alpha ? 4 : 3;
    }
    append_line(vs_buf, &vs_len, "void main() {");
    if (cc_features.opt_alpha && cc_features.diag_rdp_cvg_memory_blend) {
        append_line(vs_buf, &vs_len, "vDiagTri01 = aDiagTri01;");
        append_line(vs_buf, &vs_len, "vDiagTri2 = aDiagTri2;");
    }
    if (cc_features.opt_world_pos) {
        append_line(vs_buf, &vs_len, "vWorldPos = aWorldPos;");
    }
    if (cc_features.opt_sun_shadow) {
        append_line(vs_buf, &vs_len, "vShadowDepth = aVtxPos.w;");
    }
    if (cc_features.opt_dfdx_light) {
        append_line(vs_buf, &vs_len, "vSmoothNormal = aSmoothNormal;");
        append_line(vs_buf, &vs_len, "vLightDir = aLightDir;");
    }
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            vs_len += ge007_sprintf(vs_buf + vs_len, "vTexCoord%d = aTexCoord%d;\n", i, i);
            for (int axis = 0; axis < 2; axis++) {
                if (cc_features.clamp[i][axis]) {
                    const char axis_name = axis == 0 ? 'S' : 'T';
                    vs_len += ge007_sprintf(vs_buf + vs_len,
                                             "vTexClamp%c%d = aTexClamp%c%d;\n",
                                             axis_name, i, axis_name, i);
                }
                if (cc_features.tile_mask[i][axis]) {
                    const char axis_name = axis == 0 ? 'S' : 'T';
                    vs_len += ge007_sprintf(vs_buf + vs_len,
                                             "vTexMask%c%d = aTexMask%c%d;\n",
                                             axis_name, i, axis_name, i);
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "vFog = aFog;");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += ge007_sprintf(vs_buf + vs_len, "vInput%d = aInput%d;\n", i + 1, i + 1);
    }
    append_line(vs_buf, &vs_len, "gl_Position = aVtxPos;");
    if (!g_depth_clamp_enabled) {
        /* Depth range fix: N64 perspective maps most geometry to z/w ≈
         * 0.998-1.005, clustering the entire scene into <1% of the depth
         * buffer. Scaling clip-space z by 0.3 spreads the useful range while
         * the CPU clipper keeps triangles inside the effective frustum.
         * Skipped when GL_DEPTH_CLAMP is available (PD's preferred approach). */
        append_line(vs_buf, &vs_len, "gl_Position.z *= 0.3;");
    }
    append_line(vs_buf, &vs_len, "}");

    /* Fragment shader */
#ifdef MGB64_PORTMASTER_GLES
    append_line(fs_buf, &fs_len, "#version 320 es");
    append_line(fs_buf, &fs_len, "precision mediump float;");
    append_line(fs_buf, &fs_len, "out vec4 fragColor;");
#elif defined(__APPLE__)
    append_line(fs_buf, &fs_len, "#version 150");
    append_line(fs_buf, &fs_len, "out vec4 fragColor;");
#else
    append_line(fs_buf, &fs_len, "#version 330 core");
    append_line(fs_buf, &fs_len, "out vec4 fragColor;");
#endif
    if (cc_features.opt_world_pos) {
        append_line(fs_buf, &fs_len, "in vec3 vWorldPos;");
    }
    if (cc_features.opt_sun_shadow) {
        append_line(fs_buf, &fs_len, "in float vShadowDepth;");
    }
    if (cc_features.opt_dfdx_light) {
        append_line(fs_buf, &fs_len, "in vec3 vSmoothNormal;");
        append_line(fs_buf, &fs_len, "in vec3 vLightDir;");
        append_line(fs_buf, &fs_len, "uniform vec3 uSunColorLinear;");
        append_line(fs_buf, &fs_len, "uniform float uSunStrength;");
        append_line(fs_buf, &fs_len,
                    "vec3 mdkrSrgbToLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }");
        append_line(fs_buf, &fs_len,
                    "vec3 mdkrLinearToSrgb(vec3 c) { c = max(c, vec3(0.0)); return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }");
    }
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            fs_len += ge007_sprintf(fs_buf + fs_len, "%sin vec2 vTexCoord%d;\n",
                                     texcoord_interp, i);
            for (int axis = 0; axis < 2; axis++) {
                if (cc_features.clamp[i][axis]) {
                    const char axis_name = axis == 0 ? 'S' : 'T';
                    fs_len += ge007_sprintf(fs_buf + fs_len,
                                             "%sin float vTexClamp%c%d;\n",
                                             texcoord_interp, axis_name, i);
                }
                if (cc_features.tile_mask[i][axis]) {
                    const char axis_name = axis == 0 ? 'S' : 'T';
                    fs_len += ge007_sprintf(fs_buf + fs_len,
                                             "%sin float vTexMask%c%d;\n",
                                             texcoord_interp, axis_name, i);
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        fs_len += ge007_sprintf(fs_buf + fs_len, "%sin vec4 vFog;\n",
                                 fog_interp);
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        fs_len += ge007_sprintf(fs_buf + fs_len, "%sin vec%d vInput%d;\n",
                                 input_interp, cc_features.opt_alpha ? 4 : 3, i + 1);
    }
    if (cc_features.opt_alpha && cc_features.diag_rdp_cvg_memory_blend) {
        append_line(fs_buf, &fs_len, "noperspective in vec4 vDiagTri01;");
        append_line(fs_buf, &fs_len, "noperspective in vec2 vDiagTri2;");
    }
    if (cc_features.used_textures[0]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex0;");
    }
    if (cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex1;");
    }
    if (cc_features.opt_alpha &&
        (cc_features.diag_rdp_memory_blend || cc_features.diag_rdp_cvg_memory_blend)) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uDiagFramebuffer;");
        append_line(fs_buf, &fs_len, "uniform vec2 uDiagFramebufferOrigin;");
    }
    if (cc_features.opt_alpha && cc_features.diag_rdp_cvg_memory_blend) {
        append_line(fs_buf, &fs_len, "uniform vec4 uDiagViewport;");
    }
    /* W1.E3.T4: sun-shadow receiver uniforms (texture unit 5; §4.4). */
    if (cc_features.opt_sun_shadow) {
        append_line(fs_buf, &fs_len, "uniform sampler2DArrayShadow uShadowMap;");
        append_line(fs_buf, &fs_len, "uniform mat4 uShadowMat[2];");
        append_line(fs_buf, &fs_len, "uniform vec4 uShadowParams;");
        append_line(fs_buf, &fs_len, "uniform vec4 uShadowSplitsLayers;");
        append_line(fs_buf, &fs_len, "float mdkrShadowCascade(int cascade, float layer) {");
        append_line(fs_buf, &fs_len, "  vec4 sc = uShadowMat[cascade] * vec4(vWorldPos, 1.0);");
        append_line(fs_buf, &fs_len, "  vec3 suv = sc.xyz / sc.w * 0.5 + 0.5;");
        append_line(fs_buf, &fs_len, "  if (any(greaterThan(abs(suv - 0.5), vec3(0.5)))) return 1.0;");
        append_line(fs_buf, &fs_len, "  float sh = 0.0;");
        append_line(fs_buf, &fs_len, "  for (int dy = -1; dy <= 1; ++dy)");
        append_line(fs_buf, &fs_len, "    for (int dx = -1; dx <= 1; ++dx)");
        append_line(fs_buf, &fs_len, "      sh += texture(uShadowMap, vec4(suv.xy + vec2(dx, dy) * uShadowParams.x, layer, suv.z - uShadowParams.y));");
        /* Fade to lit over the outermost band of the map footprint instead of
         * terminating on a hard line: the far cascade ends well inside the
         * camera far plane, and a sliding shadow/lit edge on long sight
         * lines reads as an artifact. */
        append_line(fs_buf, &fs_len, "  float edge = max(abs(suv.x - 0.5), abs(suv.y - 0.5)) * 2.0;");
        append_line(fs_buf, &fs_len, "  return mix(sh / 9.0, 1.0, smoothstep(0.92, 1.0, edge));");
        append_line(fs_buf, &fs_len, "}");
    }

    if (uses_tile_mask || cc_features.n64_filter[0] || cc_features.n64_filter[1]) {
        append_line(fs_buf, &fs_len, "float n64TileMaskAxis(float texelCoord, float maskPeriod) {");
        append_line(fs_buf, &fs_len, "    float extent = abs(maskPeriod);");
        append_line(fs_buf, &fs_len, "    if (extent <= 0.5) return texelCoord;");
        append_line(fs_buf, &fs_len, "    float coord = mod(texelCoord, maskPeriod < 0.0 ? extent * 2.0 : extent);");
        append_line(fs_buf, &fs_len, "    if (maskPeriod < 0.0 && coord >= extent) coord = extent * 2.0 - coord;");
        append_line(fs_buf, &fs_len, "    return coord;");
        append_line(fs_buf, &fs_len, "}");
        append_line(fs_buf, &fs_len, "vec2 n64TileMaskUv(vec2 uv, vec2 texSize, float maskS, float maskT) {");
        append_line(fs_buf, &fs_len, "    vec2 texelCoord = uv * texSize;");
        append_line(fs_buf, &fs_len, "    texelCoord.s = n64TileMaskAxis(texelCoord.s, maskS);");
        append_line(fs_buf, &fs_len, "    texelCoord.t = n64TileMaskAxis(texelCoord.t, maskT);");
        append_line(fs_buf, &fs_len, "    return texelCoord / texSize;");
        append_line(fs_buf, &fs_len, "}");
    }

    if (cc_features.n64_filter[0] || cc_features.n64_filter[1]) {
        bool always_3point = cc_features.n64_filter_always_3point ||
            gfx_diag_n64_filter_always_3point_enabled();
        bool clamped =
            cc_features.clamp[0][0] || cc_features.clamp[0][1] ||
            cc_features.clamp[1][0] || cc_features.clamp[1][1];
        float nearest_threshold =
            gfx_diag_n64_filter_nearest_threshold(cc_features.opt_texture_edge,
                                                  clamped,
                                                  1.0f);
        append_line(fs_buf, &fs_len, "uniform vec2 uN64FilterScale;");
        append_line(fs_buf, &fs_len, "vec4 n64TextureFilter(sampler2D tex, vec2 uv, float maskS, float maskT) {");
        append_line(fs_buf, &fs_len, "    vec2 texSize = vec2(textureSize(tex, 0));");
        append_line(fs_buf, &fs_len, "    vec2 texelCoord = uv * texSize;");
        if (!always_3point) {
            append_line(fs_buf, &fs_len, "    vec2 dx = dFdx(texelCoord) * uN64FilterScale.x;");
            append_line(fs_buf, &fs_len, "    vec2 dy = dFdy(texelCoord) * uN64FilterScale.y;");
            append_line(fs_buf, &fs_len, "    vec2 footprint = max(abs(dx), abs(dy));");
            fs_len += ge007_sprintf(fs_buf + fs_len,
                                     "    if (max(footprint.x, footprint.y) < %.9f) {\n",
                                     nearest_threshold);
            append_line(fs_buf, &fs_len, "        return textureLod(tex, n64TileMaskUv((floor(texelCoord) + vec2(0.5)) / texSize, texSize, maskS, maskT), 0.0);");
            append_line(fs_buf, &fs_len, "    }");
        }
        append_line(fs_buf, &fs_len, "    vec2 offset = fract(uv * texSize - vec2(0.5));");
        append_line(fs_buf, &fs_len, "    offset -= step(1.0, offset.x + offset.y);");
        append_line(fs_buf, &fs_len, "    vec2 baseUv = uv - offset / texSize;");
        append_line(fs_buf, &fs_len, "    vec4 c0 = textureLod(tex, n64TileMaskUv(baseUv, texSize, maskS, maskT), 0.0);");
        append_line(fs_buf, &fs_len, "    vec4 c1 = textureLod(tex, n64TileMaskUv(baseUv + vec2(sign(offset.x), 0.0) / texSize, texSize, maskS, maskT), 0.0);");
        append_line(fs_buf, &fs_len, "    vec4 c2 = textureLod(tex, n64TileMaskUv(baseUv + vec2(0.0, sign(offset.y)) / texSize, texSize, maskS, maskT), 0.0);");
        append_line(fs_buf, &fs_len, "    return c0 + abs(offset.x) * (c1 - c0) + abs(offset.y) * (c2 - c0);");
        append_line(fs_buf, &fs_len, "}");
    }

    /* Declare noise function if ANY combiner input uses SHADER_NOISE,
     * or if opt_noise is set for alpha dithering. */
    bool needs_noise = cc_features.opt_noise;
    if (!needs_noise) {
        for (int ci = 0; ci < 2 && !needs_noise; ci++)
            for (int cj = 0; cj < 2 && !needs_noise; cj++)
                for (int ck = 0; ck < 4 && !needs_noise; ck++)
                    if (cc_features.c[ci][cj][ck] == SHADER_NOISE) needs_noise = true;
    }
    if (needs_noise) {
        append_line(fs_buf, &fs_len, "uniform int frame_count;");
        append_line(fs_buf, &fs_len, "uniform int window_height;");
        append_line(fs_buf, &fs_len, "float random(in vec3 value) {");
        append_line(fs_buf, &fs_len, "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));");
        append_line(fs_buf, &fs_len, "    return fract(sin(random) * 143758.5453);");
        append_line(fs_buf, &fs_len, "}");
    }
    if (cc_features.opt_alpha && cc_features.diag_rdp_cvg_memory_blend) {
        append_line(fs_buf, &fs_len, "float diagEdge(vec2 a, vec2 b, vec2 p) {");
        append_line(fs_buf, &fs_len, "    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);");
        append_line(fs_buf, &fs_len, "}");
        append_line(fs_buf, &fs_len, "float diagInsideTri(vec2 p, vec2 a, vec2 b, vec2 c) {");
        append_line(fs_buf, &fs_len, "    float e0 = diagEdge(a, b, p);");
        append_line(fs_buf, &fs_len, "    float e1 = diagEdge(b, c, p);");
        append_line(fs_buf, &fs_len, "    float e2 = diagEdge(c, a, p);");
        append_line(fs_buf, &fs_len, "    bool hasNeg = (e0 < 0.0) || (e1 < 0.0) || (e2 < 0.0);");
        append_line(fs_buf, &fs_len, "    bool hasPos = (e0 > 0.0) || (e1 > 0.0) || (e2 > 0.0);");
        append_line(fs_buf, &fs_len, "    return (hasNeg && hasPos) ? 0.0 : 1.0;");
        append_line(fs_buf, &fs_len, "}");
        append_line(fs_buf, &fs_len, "float diagCoverageSample(vec2 pixelOffset, vec2 a, vec2 b, vec2 c) {");
        append_line(fs_buf, &fs_len, "    vec2 p = ((gl_FragCoord.xy + pixelOffset - uDiagViewport.xy) / uDiagViewport.zw) * 2.0 - 1.0;");
        append_line(fs_buf, &fs_len, "    return diagInsideTri(p, a, b, c);");
        append_line(fs_buf, &fs_len, "}");
    }

    append_line(fs_buf, &fs_len, "void main() {");

    /* Shader-side UV clamping (PD pattern): clamp tex coords to the live
     * N64 tile's logical window, not blindly to the GL texture's 0..1 range. */
    for (int i = 0; i < 2; i++) {
        if (!cc_features.used_textures[i]) continue;
        fs_len += ge007_sprintf(fs_buf + fs_len,
                                 "vec2 sampleTexCoord%d = vTexCoord%d;\n",
                                 i, i);
        if (cc_features.clamp[i][0] || cc_features.clamp[i][1] ||
            cc_features.tile_mask[i][0] || cc_features.tile_mask[i][1]) {
            fs_len += ge007_sprintf(fs_buf + fs_len,
                                     "vec2 texSize%d = vec2(textureSize(uTex%d, 0));\n",
                                     i, i);
        }
        if (cc_features.clamp[i][0] || cc_features.clamp[i][1]) {
            if (cc_features.clamp[i][0] && cc_features.clamp[i][1]) {
                fs_len += ge007_sprintf(fs_buf + fs_len,
                                         "sampleTexCoord%d = clamp(vTexCoord%d, 0.5 / texSize%d, vec2(vTexClampS%d, vTexClampT%d));\n",
                                         i, i, i, i, i);
            } else if (cc_features.clamp[i][0]) {
                fs_len += ge007_sprintf(fs_buf + fs_len,
                                         "sampleTexCoord%d.s = clamp(vTexCoord%d.s, 0.5 / texSize%d.s, vTexClampS%d);\n",
                                         i, i, i, i);
            } else {
                fs_len += ge007_sprintf(fs_buf + fs_len,
                                         "sampleTexCoord%d.t = clamp(vTexCoord%d.t, 0.5 / texSize%d.t, vTexClampT%d);\n",
                                         i, i, i, i);
            }
        }
    }

    if (cc_features.used_textures[0]) {
        const char *mask_s = cc_features.tile_mask[0][0] ? "vTexMaskS0" : "0.0";
        const char *mask_t = cc_features.tile_mask[0][1] ? "vTexMaskT0" : "0.0";
        if (cc_features.n64_filter[0]) {
            fs_len += ge007_sprintf(fs_buf + fs_len,
                                    "vec4 texVal0 = n64TextureFilter(uTex0, sampleTexCoord0, %s, %s);\n",
                                    mask_s, mask_t);
        } else if (cc_features.tile_mask[0][0] || cc_features.tile_mask[0][1]) {
            fs_len += ge007_sprintf(fs_buf + fs_len,
                                    "vec4 texVal0 = texture(uTex0, n64TileMaskUv(sampleTexCoord0, texSize0, %s, %s));\n",
                                    mask_s, mask_t);
        } else {
            append_line(fs_buf, &fs_len, "vec4 texVal0 = texture(uTex0, sampleTexCoord0);");
        }
    }
    if (cc_features.used_textures[1]) {
        const char *mask_s = cc_features.tile_mask[1][0] ? "vTexMaskS1" : "0.0";
        const char *mask_t = cc_features.tile_mask[1][1] ? "vTexMaskT1" : "0.0";
        if (cc_features.n64_filter[1]) {
            fs_len += ge007_sprintf(fs_buf + fs_len,
                                    "vec4 texVal1 = n64TextureFilter(uTex1, sampleTexCoord1, %s, %s);\n",
                                    mask_s, mask_t);
        } else if (cc_features.tile_mask[1][0] || cc_features.tile_mask[1][1]) {
            fs_len += ge007_sprintf(fs_buf + fs_len,
                                    "vec4 texVal1 = texture(uTex1, n64TileMaskUv(sampleTexCoord1, texSize1, %s, %s));\n",
                                    mask_s, mask_t);
        } else {
            append_line(fs_buf, &fs_len, "vec4 texVal1 = texture(uTex1, sampleTexCoord1);");
        }
    }
    if (cc_features.opt_alpha && cc_features.diag_alpha_from_tex_intensity) {
        float mix = gfx_diag_alpha_from_tex_intensity_mix();
        if (cc_features.used_textures[0]) {
            fs_len += ge007_sprintf(fs_buf + fs_len,
                                     "texVal0.a = mix(texVal0.a, texVal0.r, %.9g);\n",
                                     mix);
        }
        if (cc_features.used_textures[1]) {
            fs_len += ge007_sprintf(fs_buf + fs_len,
                                     "texVal1.a = mix(texVal1.a, texVal1.r, %.9g);\n",
                                     mix);
        }
    }

    /* 2-cycle combiner: emit formula for each cycle.
     * SHADER_COMBINED in cycle 1 references the 'texel' variable written by cycle 0. */
    append_line(fs_buf, &fs_len, cc_features.opt_alpha ? "vec4 texel;" : "vec3 texel;");

    int num_cycles = cc_features.opt_2cyc ? 2 : 1;
    for (int cyc = 0; cyc < num_cycles; cyc++) {
        append_str(fs_buf, &fs_len, "texel = ");
        if (!cc_features.color_alpha_same[cyc] && cc_features.opt_alpha) {
            append_str(fs_buf, &fs_len, "vec4(");
            append_formula(fs_buf, &fs_len, cc_features.c[cyc],
                           cc_features.do_single[cyc][0], cc_features.do_multiply[cyc][0],
                           cc_features.do_mix[cyc][0], false, false, true);
            append_str(fs_buf, &fs_len, ", ");
            append_formula(fs_buf, &fs_len, cc_features.c[cyc],
                           cc_features.do_single[cyc][1], cc_features.do_multiply[cyc][1],
                           cc_features.do_mix[cyc][1], true, true, true);
            append_str(fs_buf, &fs_len, ")");
        } else {
            append_formula(fs_buf, &fs_len, cc_features.c[cyc],
                           cc_features.do_single[cyc][0], cc_features.do_multiply[cyc][0],
                           cc_features.do_mix[cyc][0], cc_features.opt_alpha, false,
                           cc_features.opt_alpha);
        }
        append_line(fs_buf, &fs_len, ";");

        /* Color wrapping between cycles (PD pattern) */
        if (cyc == 0 && num_cycles == 2) {
            append_line(fs_buf, &fs_len, "texel = clamp(texel, -1.01, 1.01);");
        }
        if (quantize_combiner) {
            append_line(fs_buf, &fs_len,
                        "texel = floor(clamp(texel, 0.0, 1.0) * 255.0 + 0.5) / 255.0;");
        }
    }
    /* Final clamp after all cycles */
    append_line(fs_buf, &fs_len, "texel = clamp(texel, 0.0, 1.0);");

    /*
     * RL-5: the N64 combiner finishes in authored sRGB code values. Cross the
     * explicit CO-1 boundary once, add restrained diffuse energy in linear RGB,
     * and encode back for the UNORM scene target. Interpolated model normals
     * remove the coarse per-triangle facets that rejected RL-1 supersession.
     * The combiner result—including baked vertex colour—remains the ambient
     * base; no luma replacement or divide can erase authored mood.
     */
    if (cc_features.opt_dfdx_light) {
        append_line(fs_buf, &fs_len, "vec3 smoothN = normalize(vSmoothNormal);");
        append_line(fs_buf, &fs_len, "vec3 localSun = normalize(vLightDir);");
        append_line(fs_buf, &fs_len, "float ndl = max(dot(smoothN, localSun), 0.0);");
        append_line(fs_buf, &fs_len, "vec3 litLinear = mdkrSrgbToLinear(texel.rgb);");
        append_line(fs_buf, &fs_len, "litLinear *= vec3(1.0) + uSunColorLinear * (uSunStrength * ndl);");
        append_line(fs_buf, &fs_len, "texel.rgb = clamp(mdkrLinearToSrgb(litLinear), 0.0, 1.0);");
    }

    /* W1.E3.T4: sun-shadow receiver — 3x3 PCF, injected AFTER RL-5 lighting (so
     * shadow attenuation lands on top of the recomputed lighting and shadowed areas
     * stay dark) and BEFORE the fog mix so fog always wins (lighting must not
     * brighten fog, §4.5). */
    if (cc_features.opt_sun_shadow) {
        append_line(fs_buf, &fs_len, "{");
        append_line(fs_buf, &fs_len, "  float sh = mdkrShadowCascade(0, uShadowSplitsLayers.z);");
        append_line(fs_buf, &fs_len, "  if (uShadowParams.w > 1.5) {");
        append_line(fs_buf, &fs_len, "    float farShadow = mdkrShadowCascade(1, uShadowSplitsLayers.w);");
        append_line(fs_buf, &fs_len, "    float transition = smoothstep(uShadowSplitsLayers.x, uShadowSplitsLayers.y, vShadowDepth);");
        append_line(fs_buf, &fs_len, "    sh = mix(sh, farShadow, transition);");
        append_line(fs_buf, &fs_len, "  }");
        append_line(fs_buf, &fs_len, "  texel.rgb *= mix(uShadowParams.z, 1.0, sh);");
        append_line(fs_buf, &fs_len, "}");
    }

    if (cc_features.opt_fog) {
        if (cc_features.opt_dfdx_light) {
            append_line(fs_buf, &fs_len,
                        "texel.rgb = mdkrLinearToSrgb(mix(mdkrSrgbToLinear(texel.rgb), mdkrSrgbToLinear(vFog.rgb), vFog.a));");
        } else if (cc_features.opt_alpha) {
            append_line(fs_buf, &fs_len, "texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs_buf, &fs_len, "texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }

    if (cc_features.opt_texture_edge && cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len,
                    "if (texel.a > " GFX_TEXTURE_EDGE_ALPHA_THRESHOLD_SHADER
                    ") texel.a = 1.0; else discard;");
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len, "texel.a *= floor(random(vec3(floor(gl_FragCoord.xy * (240.0 / float(window_height))), float(frame_count))) + 0.5);");
    }

    if (cc_features.diag_color_scale) {
        float scale = gfx_diag_settex_cc_color_scale_value();
        if (cc_features.opt_alpha) {
            fs_len += ge007_sprintf(fs_buf + fs_len,
                                    "texel = vec4(clamp(texel.rgb * %.9f, 0.0, 1.0), texel.a);\n",
                                    scale);
        } else {
            fs_len += ge007_sprintf(fs_buf + fs_len,
                                    "texel = clamp(texel * %.9f, 0.0, 1.0);\n",
                                    scale);
        }
    }

    if (cc_features.opt_alpha && cc_features.diag_alpha_scale) {
        float scale = gfx_diag_settex_cc_alpha_scale_value();
        fs_len += ge007_sprintf(fs_buf + fs_len,
                                "texel.a = clamp(texel.a * %.9f, 0.0, 1.0);\n",
                                scale);
    }
    if (cc_features.opt_alpha && cc_features.room_water_alpha_suppress) {
        append_line(fs_buf, &fs_len, "texel.a = 0.0;");
    }

    if (cc_features.opt_alpha && cc_features.diag_xlu_coverage_wrap_thin) {
        float rate = gfx_diag_xlu_coverage_wrap_thin_rate();
        fs_len += ge007_sprintf(fs_buf + fs_len,
                                "float coverageWrapHash = fract(sin(dot(floor(gl_FragCoord.xy), vec2(12.9898, 78.233))) * 43758.5453);\n"
                                "if (coverageWrapHash >= %.9f) discard;\n",
                                rate);
    }

    if (cc_features.opt_alpha && cc_features.diag_rdp_cvg_memory_blend) {
        append_line(fs_buf, &fs_len, "vec2 memoryUv = (gl_FragCoord.xy - uDiagFramebufferOrigin) / vec2(textureSize(uDiagFramebuffer, 0));");
        append_line(fs_buf, &fs_len, "vec4 memoryColor = texture(uDiagFramebuffer, clamp(memoryUv, vec2(0.0), vec2(1.0)));");
        append_line(fs_buf, &fs_len, "vec2 diagTri0 = vDiagTri01.xy;");
        append_line(fs_buf, &fs_len, "vec2 diagTri1 = vDiagTri01.zw;");
        append_line(fs_buf, &fs_len, "vec2 diagTri2 = vDiagTri2;");
        append_line(fs_buf, &fs_len, "float coverageCount = 0.0;");
        append_line(fs_buf, &fs_len, "coverageCount += diagCoverageSample(vec2(-0.500, -0.375), diagTri0, diagTri1, diagTri2);");
        append_line(fs_buf, &fs_len, "coverageCount += diagCoverageSample(vec2( 0.000, -0.375), diagTri0, diagTri1, diagTri2);");
        append_line(fs_buf, &fs_len, "coverageCount += diagCoverageSample(vec2(-0.250, -0.125), diagTri0, diagTri1, diagTri2);");
        append_line(fs_buf, &fs_len, "coverageCount += diagCoverageSample(vec2( 0.250, -0.125), diagTri0, diagTri1, diagTri2);");
        append_line(fs_buf, &fs_len, "coverageCount += diagCoverageSample(vec2(-0.500,  0.125), diagTri0, diagTri1, diagTri2);");
        append_line(fs_buf, &fs_len, "coverageCount += diagCoverageSample(vec2( 0.000,  0.125), diagTri0, diagTri1, diagTri2);");
        append_line(fs_buf, &fs_len, "coverageCount += diagCoverageSample(vec2(-0.250,  0.375), diagTri0, diagTri1, diagTri2);");
        append_line(fs_buf, &fs_len, "coverageCount += diagCoverageSample(vec2( 0.250,  0.375), diagTri0, diagTri1, diagTri2);");
        append_line(fs_buf, &fs_len, "if (coverageCount < 0.5) discard;");
        append_line(fs_buf, &fs_len, "float memoryCoverage = floor(floor(clamp(memoryColor.a, 0.0, 1.0) * 255.0 + 0.5) / 32.0);");
        append_line(fs_buf, &fs_len, "float coverageTotal = coverageCount + memoryCoverage;");
        append_line(fs_buf, &fs_len, "float coverageWrap = step(8.0, coverageTotal);");
        append_line(fs_buf, &fs_len, "float newCoverage = mod(coverageTotal, 8.0);");
        append_line(fs_buf, &fs_len, "float newCoverageAlpha = (newCoverage * 32.0) / 255.0;");
        append_line(fs_buf, &fs_len, "float pixelAlphaByte = floor(clamp(texel.a, 0.0, 1.0) * 255.0 + 0.5);");
        append_line(fs_buf, &fs_len, "float a0 = floor(pixelAlphaByte / 8.0);");
        append_line(fs_buf, &fs_len, "float a1 = floor((255.0 - pixelAlphaByte) / 8.0);");
        append_line(fs_buf, &fs_len, "vec3 pixelByte = floor(clamp(texel.rgb, 0.0, 1.0) * 255.0 + 0.5);");
        append_line(fs_buf, &fs_len, "vec3 memoryByte = floor(clamp(memoryColor.rgb, 0.0, 1.0) * 255.0 + 0.5);");
        append_line(fs_buf, &fs_len, "vec3 blendedByte = floor((pixelByte * a0 + memoryByte * (a1 + 1.0)) / 32.0);");
        append_line(fs_buf, &fs_len, "vec3 outByte = mix(memoryByte, blendedByte, coverageWrap);");
        append_line(fs_buf, &fs_len, "texel = vec4(clamp(outByte / 255.0, 0.0, 1.0), newCoverageAlpha);");
    } else if (cc_features.opt_alpha && cc_features.diag_rdp_memory_blend) {
        append_line(fs_buf, &fs_len, "vec2 memoryUv = (gl_FragCoord.xy - uDiagFramebufferOrigin) / vec2(textureSize(uDiagFramebuffer, 0));");
        append_line(fs_buf, &fs_len, "vec4 memoryColor = texture(uDiagFramebuffer, clamp(memoryUv, vec2(0.0), vec2(1.0)));");
        append_line(fs_buf, &fs_len, "float pixelAlphaByte = floor(clamp(texel.a, 0.0, 1.0) * 255.0 + 0.5);");
        append_line(fs_buf, &fs_len, "float a0 = floor(pixelAlphaByte / 8.0);");
        append_line(fs_buf, &fs_len, "float a1 = floor((255.0 - pixelAlphaByte) / 8.0);");
        append_line(fs_buf, &fs_len, "vec3 pixelByte = floor(clamp(texel.rgb, 0.0, 1.0) * 255.0 + 0.5);");
        append_line(fs_buf, &fs_len, "vec3 memoryByte = floor(clamp(memoryColor.rgb, 0.0, 1.0) * 255.0 + 0.5);");
        append_line(fs_buf, &fs_len, "vec3 blendedByte = floor((pixelByte * a0 + memoryByte * (a1 + 1.0)) / 32.0);");
        append_line(fs_buf, &fs_len, "texel = vec4(clamp(blendedByte / 255.0, 0.0, 1.0), 1.0);");
    }

    if (cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "fragColor = texel;");
    } else {
        append_line(fs_buf, &fs_len, "fragColor = vec4(texel, 1.0);");
    }
    if (cc_features.opt_world_pos && gfx_world_pos_diag_enabled()) {
        append_line(fs_buf, &fs_len, "fragColor = vec4(fract(vWorldPos * 0.01), 1.0);");
    }
    append_line(fs_buf, &fs_len, "}");

    vs_buf[vs_len] = '\0';
    fs_buf[fs_len] = '\0';

    /* Dump first few generated shaders */
    {
        static int shader_dump = 0;
        if (g_diag_verbose && shader_dump < 3) {
            printf("[SHADER_%d] id0=0x%016llX id1=0x%X tex=%d,%d fog=%d alpha=%d noise=%d 2cyc=%d world=%d inputs=%d\n",
                   shader_dump, (unsigned long long)shader_id0, shader_id1,
                   cc_features.used_textures[0], cc_features.used_textures[1],
                   cc_features.opt_fog, cc_features.opt_alpha, cc_features.opt_noise,
                   cc_features.opt_2cyc, cc_features.opt_world_pos, cc_features.num_inputs);
            printf("--- VS ---\n%s\n--- FS ---\n%s\n--- END ---\n", vs_buf, fs_buf);
            fflush(stdout);
            shader_dump++;
        }
    }

    const GLchar *sources[2] = { vs_buf, fs_buf };
    const GLint lengths[2] = { (GLint)vs_len, (GLint)fs_len };
    GLint success;

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &sources[0], &lengths[0]);
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char error_log[1024];
        GLint max_length = 0;
        glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &max_length);
        if (max_length > (GLint)sizeof(error_log)) max_length = sizeof(error_log) - 1;
        glGetShaderInfoLog(vertex_shader, max_length, &max_length, error_log);
        fprintf(stderr, "[fast3d] Vertex shader compilation failed:\n%s\nSource:\n%s\n", error_log, vs_buf);
        abort();
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &sources[1], &lengths[1]);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char error_log[1024];
        GLint max_length = 0;
        glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &max_length);
        if (max_length > (GLint)sizeof(error_log)) max_length = sizeof(error_log) - 1;
        glGetShaderInfoLog(fragment_shader, max_length, &max_length, error_log);
        fprintf(stderr, "[fast3d] Fragment shader compilation failed:\n%s\nSource:\n%s\n", error_log, fs_buf);
        abort();
    }

    GLuint shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    size_t cnt = 0;
    if (shader_program_pool_size >= shader_program_pool_cap) {
        int new_cap = shader_program_pool_cap ? shader_program_pool_cap * 2 : 64;
        struct ShaderProgram **grown =
            (struct ShaderProgram **)realloc(shader_program_pool, (size_t)new_cap * sizeof(struct ShaderProgram *));
        if (grown != NULL) {
            shader_program_pool = grown;
            shader_program_pool_cap = new_cap;
        }
    }
    struct ShaderProgram *prg = (struct ShaderProgram *)calloc(1, sizeof(struct ShaderProgram));
    if (prg == NULL) {
        fprintf(stderr, "[fast3d] FATAL: shader program allocation failed (OOM)\n");
        abort();
    }
    if (shader_program_pool_size < shader_program_pool_cap) {
        shader_program_pool[shader_program_pool_size++] = prg;
        /* Variant-count telemetry (T10): the pre-dbd3c06 pool silently
         * wrapped at 256 and corrupted live combiners past that point.
         * One line at process exit measures how close a real session gets.
         * Opt-in only (mirrors the GE007_BLEND_AUDIT idiom in gfx_pc.c): a
         * default run must register no atexit handler and print nothing. */
        {
            static int exit_log_registered = 0;
            if (!exit_log_registered) {
                exit_log_registered = 1;
                if (getenv("GE007_SHADER_POOL_AUDIT") != NULL) {
                    atexit(gfx_opengl_log_shader_pool_size_at_exit);
                }
            }
        }
    } else {
        /* realloc failed (OOM) above — the shader still works for this call;
         * it just won't be cached (may recompile on next use). Prefer that
         * over a crash, same tradeoff as the Metal backend's grow path. */
        fprintf(stderr, "[fast3d] WARNING: shader pool grow failed - not caching\n");
    }
    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aVtxPos");
    prg->attrib_sizes[cnt] = 4;
    ++cnt;
    if (cc_features.opt_alpha && cc_features.diag_rdp_cvg_memory_blend) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aDiagTri01");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aDiagTri2");
        prg->attrib_sizes[cnt] = 2;
        ++cnt;
    }
    if (cc_features.opt_world_pos) {
        /* Same ordinal as the VS decl + VBO pack (after diag, before textures).
         * The location may be -1 if aWorldPos is optimized out (no live consumer,
         * e.g. diag off); gfx_opengl_vertex_array_set_attribs skips the GL calls for
         * a -1 location while still advancing the stride offset by attrib_sizes=3,
         * so later attributes stay aligned. */
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aWorldPos");
        prg->attrib_sizes[cnt] = 3;
        ++cnt;
    }
    if (cc_features.opt_dfdx_light) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aSmoothNormal");
        prg->attrib_sizes[cnt] = 3;
        ++cnt;
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aLightDir");
        prg->attrib_sizes[cnt] = 3;
        ++cnt;
    }

    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            char name[16];
            ge007_sprintf(name, "aTexCoord%d", i);
            prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
            prg->attrib_sizes[cnt] = 2;
            ++cnt;
            for (int axis = 0; axis < 2; axis++) {
                if (cc_features.clamp[i][axis]) {
                    ge007_sprintf(name, "aTexClamp%c%d", axis == 0 ? 'S' : 'T', i);
                    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
                    prg->attrib_sizes[cnt] = 1;
                    ++cnt;
                }
                if (cc_features.tile_mask[i][axis]) {
                    ge007_sprintf(name, "aTexMask%c%d", axis == 0 ? 'S' : 'T', i);
                    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
                    prg->attrib_sizes[cnt] = 1;
                    ++cnt;
                }
            }
        }
    }

    if (cc_features.opt_fog) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aFog");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    for (int i = 0; i < cc_features.num_inputs; i++) {
        char name[16];
        ge007_sprintf(name, "aInput%d", i + 1);
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
        prg->attrib_sizes[cnt] = cc_features.opt_alpha ? 4 : 3;
        ++cnt;
    }

    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;
    prg->opengl_program_id = shader_program;
    prg->num_inputs = cc_features.num_inputs;
    prg->used_textures[0] = cc_features.used_textures[0];
    prg->used_textures[1] = cc_features.used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;
    prg->used_noise = needs_noise;
    /* Mirror the cutout-discard gate at the GLSL emit site (opt_texture_edge &&
     * opt_alpha); marks this program for alpha-to-coverage under MSAA. */
    prg->opt_texture_edge = cc_features.opt_texture_edge && cc_features.opt_alpha;
    if (needs_noise) {
        prg->frame_count_location = glGetUniformLocation(shader_program, "frame_count");
        prg->window_height_location = glGetUniformLocation(shader_program, "window_height");
    } else {
        prg->frame_count_location = -1;
        prg->window_height_location = -1;
    }
    prg->used_n64_filter = cc_features.n64_filter[0] || cc_features.n64_filter[1];
    prg->n64_filter_scale_location = prg->used_n64_filter
        ? glGetUniformLocation(shader_program, "uN64FilterScale")
        : -1;
    prg->diag_rdp_memory_blend = cc_features.opt_alpha && cc_features.diag_rdp_memory_blend;
    prg->diag_rdp_cvg_memory_blend = cc_features.opt_alpha && cc_features.diag_rdp_cvg_memory_blend;
    prg->diag_framebuffer_origin_location =
        (prg->diag_rdp_memory_blend || prg->diag_rdp_cvg_memory_blend)
        ? glGetUniformLocation(shader_program, "uDiagFramebufferOrigin")
        : -1;
    prg->diag_viewport_location = prg->diag_rdp_cvg_memory_blend
        ? glGetUniformLocation(shader_program, "uDiagViewport")
        : -1;
    prg->opt_sun_shadow = cc_features.opt_sun_shadow;
    if (prg->opt_sun_shadow) {
        prg->shadow_mat_location =
            glGetUniformLocation(shader_program, "uShadowMat[0]");
        prg->shadow_params_location =
            glGetUniformLocation(shader_program, "uShadowParams");
        prg->shadow_splits_layers_location =
            glGetUniformLocation(shader_program, "uShadowSplitsLayers");
    } else {
        prg->shadow_mat_location =
            prg->shadow_params_location =
            prg->shadow_splits_layers_location = -1;
    }
    prg->opt_dfdx_light = cc_features.opt_dfdx_light;
    if (cc_features.opt_dfdx_light) {
        prg->sun_color_location = glGetUniformLocation(shader_program, "uSunColorLinear");
        prg->sun_strength_location = glGetUniformLocation(shader_program, "uSunStrength");
    } else {
        prg->sun_color_location = -1;
        prg->sun_strength_location = -1;
    }

    gfx_opengl_load_shader(prg);

    if (cc_features.used_textures[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex0");
        glUniform1i(sampler_location, 0);
    }
    if (cc_features.used_textures[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex1");
        glUniform1i(sampler_location, 1);
    }
    if (cc_features.opt_alpha &&
        (cc_features.diag_rdp_memory_blend || cc_features.diag_rdp_cvg_memory_blend)) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uDiagFramebuffer");
        glUniform1i(sampler_location, 2);
    }
    if (cc_features.opt_sun_shadow) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uShadowMap");
        glUniform1i(sampler_location, 5);   /* shadow map on unit 5 (§4.4) */
    }

    return prg;
}

static struct ShaderProgram *gfx_opengl_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    for (int i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i]->shader_id0 == shader_id0 && shader_program_pool[i]->shader_id1 == shader_id1) {
            return shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_opengl_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static GLuint gfx_opengl_new_texture(void) {
    GLuint ret;
    glGenTextures(1, &ret);
    return ret;
}

static void gfx_opengl_delete_texture(GLuint texture_id) {
    if (texture_id != 0) {
        glDeleteTextures(1, &texture_id);
    }
}

/* NATIVE_PORT (mdkr64): which texture ids carry a full mip chain, and which
 * texture is bound per tile. set_sampler_parameters runs per draw and would
 * otherwise reset MIN_FILTER to a non-mipmap value on every bind. */
#define GFX_GL_MAX_TRACKED_TEX 8192
static uint8_t s_gl_tex_has_mips[GFX_GL_MAX_TRACKED_TEX];
static GLuint  s_gl_bound_tex[2];

static void gfx_gl_set_has_mips(GLuint id, int has) {
    if (id < GFX_GL_MAX_TRACKED_TEX) {
        s_gl_tex_has_mips[id] = (uint8_t) (has ? 1 : 0);
    }
}

static int gfx_gl_has_mips(GLuint id) {
    return (id < GFX_GL_MAX_TRACKED_TEX) ? s_gl_tex_has_mips[id] : 0;
}

static void gfx_opengl_select_texture(int tile, GLuint texture_id) {
    glActiveTexture(GL_TEXTURE0 + tile);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    if (tile >= 0 && tile < 2) {
        s_gl_bound_tex[tile] = texture_id;
    }
}

/* NATIVE_PORT (mdkr64): upload a CPU-built chain. glGenerateMipmap is
 * deliberately not called — it is the call that fails silently on NPOT under
 * macOS Metal and caused mipmaps to be disabled here in the first place. */
static bool gfx_opengl_upload_texture_mipped(const uint8_t *const *level_rgba,
                                             const int *level_w, const int *level_h,
                                             int level_count) {
    GLint bound = 0;
    if (level_rgba == NULL || level_w == NULL || level_h == NULL || level_count <= 0) {
        return false;
    }
    static int unpack_set_mip = 0;
    if (!unpack_set_mip) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        unpack_set_mip = 1;
    }
    while (glGetError() != GL_NO_ERROR) {
    }
    for (int l = 0; l < level_count; l++) {
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, level_w[l], level_h[l], 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, level_rgba[l]);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level_count - 1);
    if (glGetError() != GL_NO_ERROR) {
        return false;
    }
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    gfx_gl_set_has_mips((GLuint) bound, 1);
    return true;
}

static bool gfx_opengl_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    if (rgba32_buf == NULL || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return false;
    }
    static int unpack_set = 0;
    if (!unpack_set) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        unpack_set = 1;
    }
    while (glGetError() != GL_NO_ERROR) {
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    {
        /* Texture ids are recycled by the HLE cache; a single-level upload into
         * a slot that previously held a chain must clear the flag or the sampler
         * will ask for levels that no longer exist. */
        GLint bound_single = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_single);
        gfx_gl_set_has_mips((GLuint) bound_single, 0);
    }
    /* Treat every uploaded texture as a single-level image. Metal-backed GL is
     * particularly sensitive to incomplete mip state on frontend NPOT uploads
     * such as the 440x1 eye-intro strips, and will silently substitute a zero
     * texture when the object is considered unloadable. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "[GL-TEX-UPLOAD-ERR] width=%d height=%d err=0x%x\n", width, height, err);
        texDebugDumpRecentFireEvents(stderr);
        return false;
    }
    GLint actual_width = 0;
    GLint actual_height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actual_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actual_height);
    err = glGetError();
    if (err != GL_NO_ERROR || actual_width != width || actual_height != height) {
        fprintf(stderr,
                "[GL-TEX-UPLOAD-BAD] width=%d height=%d actual=%d,%d err=0x%x\n",
                width, height, actual_width, actual_height, err);
        texDebugDumpRecentFireEvents(stderr);
        return false;
    }
    return true;
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP) {
        return GL_CLAMP_TO_EDGE;
    }
    return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    glActiveTexture(GL_TEXTURE0 + tile);
    /* Use non-mipmap filters.  On macOS Metal, NPOT textures (32x48, etc.)
     * can fail glGenerateMipmap silently, leaving the texture incomplete.
     * The driver then substitutes a "zero texture" → garbage output.
     * GL_LINEAR/GL_NEAREST without mipmaps avoids this entirely. */
    {
        /* NATIVE_PORT (mdkr64): a texture that carries a chain gets a mipmap
         * MIN_FILTER; everything else keeps the historical single-level filter.
         * The comment above describes why mips were off, not why they must
         * stay off — the chain is CPU-built now and glGenerateMipmap is never
         * called, so the NPOT failure it describes cannot occur. */
        int mipped = (tile >= 0 && tile < 2) && gfx_gl_has_mips(s_gl_bound_tex[tile]) &&
                     !g_gfxSamplerLod0Only;
        GLint min_filter;
        if (mipped) {
            min_filter = linear_filter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
        } else {
            min_filter = linear_filter ? GL_LINEAR : GL_NEAREST;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
    /* Enable anisotropic filtering if available — critical for textures
     * that tile many times at oblique angles (N64 room/terrain surfaces) */
    static float max_aniso = -1;
    if (max_aniso < 0) {
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_aniso);
        if (max_aniso <= 0) max_aniso = 1;
    }
    if (max_aniso > 1) {
        /* NATIVE_PORT (mdkr64): honour Video.AnisotropicFiltering. This used to
         * force the hardware maximum whenever linear filtering was on, which
         * made GL disagree with WebGPU (which reads g_pcTextureAnisotropy) and
         * left Pure mode filtering unfaithfully. */
        float want = (float) (g_pcTextureAnisotropy < 1 ? 1 : g_pcTextureAnisotropy);
        if (want > max_aniso) {
            want = max_aniso;
        }
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                        linear_filter ? want : 1.0f);
    }
}

/* Unified depth mode — matches PD's set_depth_mode pattern.
 * Separates depth test enable from depth comparison function,
 * handles all four N64 zmodes, and respects prim depth source. */
static void gfx_opengl_set_depth_mode(bool depth_test, bool depth_update,
                                       bool depth_compare, bool depth_source_prim,
                                       uint16_t zmode) {
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(depth_update ? GL_TRUE : GL_FALSE);

        if (depth_compare) {
            switch (zmode) {
                case 0x400: /* ZMODE_INTER */
                    glDepthFunc(GL_LEQUAL);
                    glDisable(GL_POLYGON_OFFSET_FILL);
                    glPolygonOffset(0, 0);
                    break;
                case 0:     /* ZMODE_OPA */
                case 0x800: /* ZMODE_XLU */
                    /* N64 room geometry frequently lands on equal-depth seams
                     * after CPU clipping. Using GL_LESS here leaves thin gaps
                     * where later coplanar/clipped fragments fail the compare,
                     * showing the "blue shard" leaks seen in interior levels. */
                    glDepthFunc((zmode == 0x800 &&
                                 gfx_diag_zmode_xlu_less_enabled()) ?
                                GL_LESS : GL_LEQUAL);
                    glDisable(GL_POLYGON_OFFSET_FILL);
                    glPolygonOffset(0, 0);
                    break;
                case 0xc00: /* ZMODE_DEC */
                    glDepthFunc(gfx_diag_zmode_dec_less_enabled() ?
                                GL_LESS : GL_LEQUAL);
                    if (gfx_diag_zmode_dec_no_poly_offset_enabled()) {
                        glDisable(GL_POLYGON_OFFSET_FILL);
                        glPolygonOffset(0, 0);
                    } else {
                        float factor;
                        float units;
                        gfx_diag_zmode_dec_offset_values(&factor, &units);
                        glEnable(GL_POLYGON_OFFSET_FILL);
                        glPolygonOffset(factor, units);
                    }
                    break;
            }
        } else {
            /* No depth compare: write depth but always pass.
             * Critical for surfaces that prime the depth buffer
             * without testing against it. */
            glDepthFunc(GL_ALWAYS);
            glDisable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(0, 0);
        }
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
    current_height = height;
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

static void gfx_opengl_set_shadow_view(int view_index) {
    if (view_index == g_shadow_receiver_view) {
        return;
    }
    g_shadow_receiver_view = view_index;
    if (current_shader_program != NULL &&
        current_shader_program->opt_sun_shadow) {
        gfx_opengl_set_uniforms(current_shader_program);
    }
}

static void gfx_opengl_set_blend_mode(enum GfxBlendMode mode) {
    if (mode == GFX_BLEND_DISABLED ||
        mode == GFX_BLEND_ALPHA_RDP_MEMORY ||
        mode == GFX_BLEND_ALPHA_RDP_CVG_MEMORY) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        if (mode == GFX_BLEND_MODULATE) {
            glBlendFunc(GL_DST_COLOR, GL_ZERO);
        } else {
            switch (gfx_diag_alpha_blend_mode()) {
                case 1:
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    break;
                case 2:
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                    break;
                case 3:
                    glBlendFunc(GL_ONE, GL_ZERO);
                    break;
                case 4:
                    glBlendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);
                    break;
                default:
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    break;
            }
        }
    }
    if (mode == GFX_BLEND_ALPHA_COVERAGE && !g_diag_alpha_coverage_logged) {
        fprintf(stderr,
                "[fast3d] DIAG alpha coverage blend active; requires Video.MSAA>0 "
                "for GL_SAMPLE_ALPHA_TO_COVERAGE to affect output\n");
        fflush(stderr);
        g_diag_alpha_coverage_logged = 1;
    }
    if (mode == GFX_BLEND_ALPHA_CVG_WRAP_STENCIL &&
        !g_diag_xlu_coverage_stencil_logged) {
        fprintf(stderr,
                "[fast3d] DIAG XLU coverage stencil blend active; "
                "requires stencil-backed scene target\n");
        fflush(stderr);
        g_diag_xlu_coverage_stencil_logged = 1;
    }
    if (mode == GFX_BLEND_ALPHA_RDP_MEMORY &&
        !g_diag_xlu_rdp_memory_blend_logged) {
        fprintf(stderr,
                "[fast3d] XLU RDP memory blend active; "
                "shader samples framebuffer memory color\n");
        fflush(stderr);
        g_diag_xlu_rdp_memory_blend_logged = 1;
    }
    if (mode == GFX_BLEND_ALPHA_RDP_CVG_MEMORY &&
        !g_diag_xlu_rdp_cvg_memory_blend_logged) {
        fprintf(stderr,
                "[fast3d] XLU RDP coverage memory blend active; "
                "shader samples framebuffer memory color and tracks coverage alpha\n");
        fflush(stderr);
        g_diag_xlu_rdp_cvg_memory_blend_logged = 1;
    }
    /* Default A2C must not engage on blended draws (coverage*srcAlpha double-applies).
     * GFX_BLEND_ALPHA_COVERAGE is a default-off diagnostic override. */
    g_blend_disabled = (mode == GFX_BLEND_DISABLED);
    g_blend_alpha_coverage = (mode == GFX_BLEND_ALPHA_COVERAGE);
    g_blend_alpha_cvg_wrap_stencil = (mode == GFX_BLEND_ALPHA_CVG_WRAP_STENCIL);
    g_blend_alpha_rdp_memory = (mode == GFX_BLEND_ALPHA_RDP_MEMORY);
    g_blend_alpha_rdp_cvg_memory = (mode == GFX_BLEND_ALPHA_RDP_CVG_MEMORY);
    /*
     * The coverage-memory shader stores its synthetic 3-bit coverage in the
     * scene-target alpha channel. Ordinary GL alpha blending would otherwise
     * rewrite that channel with opacity history, so preserve it for non-RDP
     * blended color draws and let opaque/RDP-memory draws explicitly write it.
     */
    {
        bool preserve_coverage_alpha =
            g_scene_target_bound &&
            (gfx_opengl_room_xlu_cvg_memory_enabled() ||
             gfx_diag_xlu_rdp_cvg_memory_blend_enabled()) &&
            (mode == GFX_BLEND_ALPHA ||
             mode == GFX_BLEND_MODULATE ||
             mode == GFX_BLEND_ALPHA_COVERAGE ||
             mode == GFX_BLEND_ALPHA_CVG_WRAP_STENCIL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE,
                    preserve_coverage_alpha ? GL_FALSE : GL_TRUE);
    }
    gfx_opengl_update_a2c_state();
}

static GLuint g_diag_framebuffer_snapshot_tex;
static int g_diag_framebuffer_snapshot_w;
static int g_diag_framebuffer_snapshot_h;

static bool gfx_opengl_ensure_framebuffer_snapshot_texture(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (g_diag_framebuffer_snapshot_tex == 0) {
        glGenTextures(1, &g_diag_framebuffer_snapshot_tex);
    }
    glBindTexture(GL_TEXTURE_2D, g_diag_framebuffer_snapshot_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    if (g_diag_framebuffer_snapshot_w != width ||
        g_diag_framebuffer_snapshot_h != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g_diag_framebuffer_snapshot_w = width;
        g_diag_framebuffer_snapshot_h = height;
    }
    return true;
}

static void gfx_opengl_clamp_snapshot_rect(const GLint viewport[4],
                                           const GLint requested_rect[4],
                                           GLint out_rect[4]) {
    GLint copy_x = requested_rect != NULL ? requested_rect[0] : viewport[0];
    GLint copy_y = requested_rect != NULL ? requested_rect[1] : viewport[1];
    GLint copy_w = requested_rect != NULL ? requested_rect[2] : viewport[2];
    GLint copy_h = requested_rect != NULL ? requested_rect[3] : viewport[3];
    GLint viewport_x1 = viewport[0] + viewport[2];
    GLint viewport_y1 = viewport[1] + viewport[3];
    GLint copy_x1 = copy_x + copy_w;
    GLint copy_y1 = copy_y + copy_h;

    if (copy_x < viewport[0]) copy_x = viewport[0];
    if (copy_y < viewport[1]) copy_y = viewport[1];
    if (copy_x1 > viewport_x1) copy_x1 = viewport_x1;
    if (copy_y1 > viewport_y1) copy_y1 = viewport_y1;

    out_rect[0] = copy_x;
    out_rect[1] = copy_y;
    out_rect[2] = copy_x1 > copy_x ? copy_x1 - copy_x : 0;
    out_rect[3] = copy_y1 > copy_y ? copy_y1 - copy_y : 0;
}

static bool gfx_opengl_copy_framebuffer_snapshot(const GLint viewport[4],
                                                 const GLint requested_rect[4]) {
    GLint saved_active_texture = 0;
    GLint saved_texture2 = 0;
    GLint saved_read_fbo = 0;
    GLint saved_draw_fbo = 0;
    GLint saved_read_buffer = GL_BACK;
    GLboolean saved_scissor = GL_FALSE;
    GLuint read_fbo;
    GLint copy_rect[4];
    GLint dst_x;
    GLint dst_y;

    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active_texture);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture2);
    if (!gfx_opengl_ensure_framebuffer_snapshot_texture(viewport[2], viewport[3])) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)saved_texture2);
        glActiveTexture((GLenum)saved_active_texture);
        return false;
    }
    gfx_opengl_clamp_snapshot_rect(viewport, requested_rect, copy_rect);
    if (copy_rect[2] <= 0 || copy_rect[3] <= 0) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)saved_texture2);
        glActiveTexture((GLenum)saved_active_texture);
        return false;
    }
    dst_x = copy_rect[0] - viewport[0];
    dst_y = copy_rect[1] - viewport[1];

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw_fbo);
    glGetIntegerv(GL_READ_BUFFER, &saved_read_buffer);
    saved_scissor = glIsEnabled(GL_SCISSOR_TEST);

    read_fbo = (GLuint)saved_draw_fbo;
    /* RDP memory-blend shaders sample the previous framebuffer color through a
     * normal 2D texture. If gameplay is rendering into the MSAA scene target,
     * resolve color first; glCopyTexSubImage2D from a multisample read FBO is
     * driver-dependent and can fail exactly on the translucent materials this
     * path exists to emulate. */
    if (g_scene_target_bound &&
        g_scene_target_multisampled &&
        g_scene_msaa_fbo != 0 &&
        g_scene_fbo != 0 &&
        read_fbo == g_scene_msaa_fbo) {
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_msaa_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_scene_fbo);
        glBlitFramebuffer(copy_rect[0], copy_rect[1],
                          copy_rect[0] + copy_rect[2],
                          copy_rect[1] + copy_rect[3],
                          copy_rect[0], copy_rect[1],
                          copy_rect[0] + copy_rect[2],
                          copy_rect[1] + copy_rect[3],
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        read_fbo = g_scene_fbo;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glReadBuffer(read_fbo != 0 ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, dst_x, dst_y,
                        copy_rect[0], copy_rect[1],
                        copy_rect[2], copy_rect[3]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)saved_draw_fbo);
    glReadBuffer((GLenum)saved_read_buffer);
    if (saved_scissor) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }

    if (current_shader_program != NULL &&
        current_shader_program->diag_framebuffer_origin_location >= 0) {
        glUniform2f(current_shader_program->diag_framebuffer_origin_location,
                    (float)viewport[0], (float)viewport[1]);
    }
    if (current_shader_program != NULL &&
        current_shader_program->diag_viewport_location >= 0) {
        glUniform4f(current_shader_program->diag_viewport_location,
                    (float)viewport[0], (float)viewport[1],
                    (float)viewport[2], (float)viewport[3]);
    }

    glBindTexture(GL_TEXTURE_2D, g_diag_framebuffer_snapshot_tex);
    glActiveTexture((GLenum)saved_active_texture);
    return true;
}

static bool gfx_opengl_compute_tri_snapshot_rect(const float buf_vbo[],
                                                 size_t buf_vbo_len,
                                                 size_t buf_vbo_num_tris,
                                                 size_t tri,
                                                 const GLint viewport[4],
                                                 GLint out_rect[4]) {
    size_t vertex_count;
    size_t stride;
    const float *base;
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    /* The shader samples framebuffer memory via gl_FragCoord, so only pixels
     * inside this triangle's screen-space bbox can be read for this draw. */
    const int margin = 3;

    if (buf_vbo == NULL || buf_vbo_num_tris == 0 || tri >= buf_vbo_num_tris ||
        viewport[2] <= 0 || viewport[3] <= 0) {
        return false;
    }

    vertex_count = buf_vbo_num_tris * 3;
    if (vertex_count == 0 || buf_vbo_len % vertex_count != 0) {
        return false;
    }

    stride = buf_vbo_len / vertex_count;
    if (stride < 10) {
        return false;
    }

    base = &buf_vbo[tri * 3 * stride];
    for (int i = 0; i < 3; i++) {
        float ndc_x = base[4 + i * 2 + 0];
        float ndc_y = base[4 + i * 2 + 1];
        float pixel_x;
        float pixel_y;

        if (ndc_x != ndc_x || ndc_y != ndc_y ||
            ndc_x <= -100000.0f || ndc_x >= 100000.0f ||
            ndc_y <= -100000.0f || ndc_y >= 100000.0f) {
            return false;
        }

        pixel_x = (float)viewport[0] + (ndc_x * 0.5f + 0.5f) * (float)viewport[2];
        pixel_y = (float)viewport[1] + (ndc_y * 0.5f + 0.5f) * (float)viewport[3];

        if (i == 0) {
            min_x = max_x = pixel_x;
            min_y = max_y = pixel_y;
        } else {
            if (pixel_x < min_x) min_x = pixel_x;
            if (pixel_x > max_x) max_x = pixel_x;
            if (pixel_y < min_y) min_y = pixel_y;
            if (pixel_y > max_y) max_y = pixel_y;
        }
    }

    out_rect[0] = (GLint)floorf(min_x) - margin;
    out_rect[1] = (GLint)floorf(min_y) - margin;
    out_rect[2] = (GLint)ceilf(max_x) + margin - out_rect[0];
    out_rect[3] = (GLint)ceilf(max_y) + margin - out_rect[1];
    gfx_opengl_clamp_snapshot_rect(viewport, out_rect, out_rect);

    return out_rect[2] > 0 && out_rect[3] > 0;
}

static bool gfx_opengl_rdp_cvg_snapshot_rects_enabled(void) {
    static int enabled = -1;

    if (enabled < 0) {
        const char *disable_env = getenv("GE007_DISABLE_RDP_CVG_SNAPSHOT_RECTS");
        const char *enable_env = getenv("GE007_RDP_CVG_SNAPSHOT_RECTS");

        enabled = 1;
        if ((disable_env != NULL && disable_env[0] != '\0' && disable_env[0] != '0') ||
            (enable_env != NULL && enable_env[0] == '0')) {
            enabled = 0;
        }
    }

    return enabled != 0;
}

/* Union of every triangle's screen-space snapshot rect in the batch. Returns
 * false (→ caller copies the full viewport) if any triangle is degenerate, so
 * the snapshot region always covers at least what each triangle would sample. */
static bool gfx_opengl_compute_batch_snapshot_rect(const float buf_vbo[],
                                                   size_t buf_vbo_len,
                                                   size_t buf_vbo_num_tris,
                                                   const GLint viewport[4],
                                                   GLint out_rect[4]) {
    GLint acc[4] = {0, 0, 0, 0};
    bool any = false;

    for (size_t tri = 0; tri < buf_vbo_num_tris; tri++) {
        GLint r[4];
        if (!gfx_opengl_compute_tri_snapshot_rect(buf_vbo, buf_vbo_len,
                                                  buf_vbo_num_tris, tri,
                                                  viewport, r)) {
            return false;
        }
        if (!any) {
            acc[0] = r[0]; acc[1] = r[1]; acc[2] = r[2]; acc[3] = r[3];
            any = true;
        } else {
            GLint x0 = acc[0] < r[0] ? acc[0] : r[0];
            GLint y0 = acc[1] < r[1] ? acc[1] : r[1];
            GLint x1 = (acc[0] + acc[2]) > (r[0] + r[2]) ? (acc[0] + acc[2]) : (r[0] + r[2]);
            GLint y1 = (acc[1] + acc[3]) > (r[1] + r[3]) ? (acc[1] + acc[3]) : (r[1] + r[3]);
            acc[0] = x0; acc[1] = y0; acc[2] = x1 - x0; acc[3] = y1 - y0;
        }
    }
    if (!any) {
        return false;
    }
    out_rect[0] = acc[0]; out_rect[1] = acc[1];
    out_rect[2] = acc[2]; out_rect[3] = acc[3];
    return out_rect[2] > 0 && out_rect[3] > 0;
}

enum GfxXluSnapshotMode {
    GFX_XLU_SNAPSHOT_PER_BATCH = 0,  /* one framebuffer copy per draw batch (default) */
    GFX_XLU_SNAPSHOT_PER_TRI   = 1,  /* legacy: one copy per triangle (exact-parity A/B) */
};

/* Granularity of the RDP-memory-blend framebuffer snapshot. Default per-batch
 * removes the per-triangle GPU pipeline stall (docs/design/PERFORMANCE_PLAN.md M1);
 * GE007_XLU_SNAPSHOT_MODE=pertri restores the legacy path for A/B. */
static enum GfxXluSnapshotMode gfx_opengl_xlu_snapshot_mode(void) {
    static int mode = -1;

    if (mode < 0) {
        const char *env = getenv("GE007_XLU_SNAPSHOT_MODE");
        mode = GFX_XLU_SNAPSHOT_PER_BATCH;
        if (env != NULL && env[0] != '\0' &&
            (strcmp(env, "pertri") == 0 || strcmp(env, "per-tri") == 0 ||
             strcmp(env, "tri") == 0 || strcmp(env, "1") == 0)) {
            mode = GFX_XLU_SNAPSHOT_PER_TRI;
        }
    }
    return (enum GfxXluSnapshotMode)mode;
}

static void gfx_opengl_draw_triangles_cvg_wrap_stencil(size_t buf_vbo_num_tris) {
    GLboolean saved_color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean saved_depth_mask = GL_FALSE;
    GLboolean saved_stencil = glIsEnabled(GL_STENCIL_TEST);
    int increment = gfx_diag_xlu_coverage_stencil_increment();
    int threshold = 8 - increment;

    glGetBooleanv(GL_COLOR_WRITEMASK, saved_color_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &saved_depth_mask);

    glEnable(GL_STENCIL_TEST);
    for (size_t tri = 0; tri < buf_vbo_num_tris; tri++) {
        GLint first = (GLint)(tri * 3);

        glColorMask(saved_color_mask[0], saved_color_mask[1],
                    saved_color_mask[2], saved_color_mask[3]);
        glDepthMask(saved_depth_mask);
        glStencilMask(0x00);
        glStencilFunc(GL_LEQUAL, threshold, 0x07);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glDrawArrays(GL_TRIANGLES, first, 3);

        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_FALSE);
        glStencilMask(0x07);
        glStencilFunc(GL_ALWAYS, 0, 0x07);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR_WRAP);
        for (int i = 0; i < increment; i++) {
            glDrawArrays(GL_TRIANGLES, first, 3);
        }
    }

    glColorMask(saved_color_mask[0], saved_color_mask[1],
                saved_color_mask[2], saved_color_mask[3]);
    glDepthMask(saved_depth_mask);
    glStencilMask(0xff);
    glStencilFunc(GL_ALWAYS, 0, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    if (!saved_stencil) {
        glDisable(GL_STENCIL_TEST);
    }
}

static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (current_shader_program != NULL) {
        gfx_opengl_set_uniforms(current_shader_program);
    }
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    if (g_blend_alpha_rdp_memory || g_blend_alpha_rdp_cvg_memory) {
        GLint viewport[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, viewport);
        if (gfx_opengl_xlu_snapshot_mode() == GFX_XLU_SNAPSHOT_PER_TRI) {
            /* Legacy exact-parity path: re-snapshot the framebuffer before every
             * triangle. Correct for intra-batch overlap but issues one
             * framebuffer copy + pipeline stall per triangle — the Tier-A
             * bottleneck. Kept as GE007_XLU_SNAPSHOT_MODE=pertri for A/B. */
            for (size_t tri = 0; tri < buf_vbo_num_tris; tri++) {
                GLint snapshot_rect[4];
                const GLint *requested_rect = NULL;
                if (g_blend_alpha_rdp_cvg_memory &&
                    gfx_opengl_rdp_cvg_snapshot_rects_enabled() &&
                    gfx_opengl_compute_tri_snapshot_rect(buf_vbo, buf_vbo_len,
                                                         buf_vbo_num_tris, tri,
                                                         viewport, snapshot_rect)) {
                    requested_rect = snapshot_rect;
                }
                (void)gfx_opengl_copy_framebuffer_snapshot(viewport, requested_rect);
                glDrawArrays(GL_TRIANGLES, (GLint)(tri * 3), 3);
            }
        } else {
            /* Default per-batch path: one framebuffer snapshot (union of the
             * batch's triangle rects) then a single draw. The snapshot region
             * covers every triangle's sampled pixels, so each fragment reads the
             * same framebuffer memory it would under the per-triangle path,
             * except for triangles that overlap *within this one same-material
             * batch* (glass panes and foliage cards, which are coplanar /
             * non-overlapping). Removes the per-triangle stall. See
             * docs/design/PERFORMANCE_PLAN.md M1. */
            GLint batch_rect[4];
            const GLint *requested_rect = NULL;
            if (g_blend_alpha_rdp_cvg_memory &&
                gfx_opengl_rdp_cvg_snapshot_rects_enabled() &&
                gfx_opengl_compute_batch_snapshot_rect(buf_vbo, buf_vbo_len,
                                                       buf_vbo_num_tris,
                                                       viewport, batch_rect)) {
                requested_rect = batch_rect;
            }
            (void)gfx_opengl_copy_framebuffer_snapshot(viewport, requested_rect);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(3 * buf_vbo_num_tris));
        }
    } else if (g_blend_alpha_cvg_wrap_stencil && gfx_diag_xlu_coverage_stencil_enabled()) {
        gfx_opengl_draw_triangles_cvg_wrap_stencil(buf_vbo_num_tris);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
    }
}

static int g_diag_output_vi_filter_checked;
static int g_diag_output_vi_filter_enabled;
static int g_diag_output_vi_filter_disabled;
static int g_diag_output_vi_filter_w;
static int g_diag_output_vi_filter_h;
static int g_auto_menu_vi_filter_checked;
static int g_auto_menu_vi_filter_disabled;
static int g_auto_menu_vi_filter_logged;
static int g_auto_gameplay_vi_filter_checked;
static int g_auto_gameplay_vi_filter_enabled;
static int g_auto_gameplay_vi_filter_disabled;
static int g_auto_gameplay_vi_filter_logged;
static int g_diag_output_vi_logical_checked;
static int g_diag_output_vi_logical_enabled;
static int g_diag_output_vi_logical_w;
static int g_diag_output_vi_logical_h;
static GLuint g_output_filter_copy_tex;
static GLuint g_output_filter_low_tex;
static GLuint g_output_filter_logical_tex;
static GLuint g_output_filter_fbo;
static GLuint g_output_filter_program;
static GLuint g_output_filter_vao;
static int g_output_filter_copy_w;
static int g_output_filter_copy_h;
static int g_output_filter_low_w;
static int g_output_filter_low_h;
static int g_output_filter_logical_w;
static int g_output_filter_logical_h;
static int g_diag_output_filter_color_checked;
static float g_diag_output_filter_color_scale = 1.0f;
static float g_diag_output_filter_color_bias;
static int g_diag_output_rgb555_checked; /* GE007_DIAG_OUTPUT_RGB555=1|dither */
static int g_diag_output_rgb555_mode;
static bool gfx_opengl_read_framebuffer_rgb(int x, int y, int width, int height, uint8_t *rgb_out) {
    GLint saved_pack_alignment = 4;
    GLint saved_read_fbo = 0;
    GLint saved_draw_fbo = 0;
    GLint saved_read_buffer = GL_BACK;
    GLboolean saved_scissor = GL_FALSE;
    GLuint read_fbo;

    if (rgb_out == NULL || width <= 0 || height <= 0) {
        return false;
    }

    glGetIntegerv(GL_PACK_ALIGNMENT, &saved_pack_alignment);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw_fbo);
    glGetIntegerv(GL_READ_BUFFER, &saved_read_buffer);
    saved_scissor = glIsEnabled(GL_SCISSOR_TEST);

    read_fbo = (GLuint)saved_draw_fbo;
    if (g_scene_target_bound && g_scene_target_multisampled) {
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_msaa_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_scene_fbo);
        glBlitFramebuffer(0, 0, g_scene_w, g_scene_h,
                          0, 0, g_scene_w, g_scene_h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        read_fbo = g_scene_fbo;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glReadBuffer(read_fbo != 0 ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb_out);
    GLenum err = glGetError();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)saved_draw_fbo);
    glReadBuffer((GLenum)saved_read_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, saved_pack_alignment);
    if (saved_scissor) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }

    return err == GL_NO_ERROR;
}

/* ---------------------------------------------------------------------------
 * AUDIT-0040 / F-28: defined completed-frame capture.
 *
 * Desktop GL reads the front buffer (glReadBuffer(GL_FRONT)) because the manual
 * BMP capture runs after the previous frame's swap, when the back buffer is
 * undefined. GLES has no GL_FRONT for the default framebuffer, so that path read
 * the stale/undefined back buffer. The fix: while the composited final frame is
 * still bound to the default framebuffer — after gfx_rapi->end_frame() resolve +
 * output VI filter + the minimap draw, and BEFORE the swap in gfx_end_frame —
 * stash it into a persistent RGB buffer. The native `--dump-frames` path also
 * consumes that buffer because DKR can present a VI without submitting a new
 * graphics task; sampling GL_BACK after each swap alternated valid content with
 * undefined/older storage.
 * glReadPixels on the bound default framebuffer BEFORE the swap is defined in
 * GLES3 (unlike a post-swap back-buffer read), so this is GLES-safe.
 *
 * `gfx_opengl_end_frame` invokes this only while diagnostic dumping is armed,
 * after a graphics task completed. Normal play pays no readback cost.
 * ------------------------------------------------------------------------- */
static uint8_t *g_capture_frame_buf = NULL;
static int g_capture_frame_w = 0;
static int g_capture_frame_h = 0;
static bool g_capture_frame_valid = false;

void gfx_opengl_capture_default_framebuffer(void) {
    extern SDL_Window *g_sdlWindow;
    int w = 0, h = 0;
    GLint saved_read_fbo = 0;
    GLint saved_read_buffer = GL_BACK;
    GLint saved_pack_alignment = 4;
    GLboolean saved_scissor = GL_FALSE;

    g_capture_frame_valid = false;

    if (g_sdlWindow != NULL) {
        SDL_GL_GetDrawableSize(g_sdlWindow, &w, &h);
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    if (g_capture_frame_buf == NULL || w != g_capture_frame_w || h != g_capture_frame_h) {
        uint8_t *nb = (uint8_t *)realloc(g_capture_frame_buf, (size_t)w * (size_t)h * 3U);
        if (nb == NULL) {
            return;
        }
        g_capture_frame_buf = nb;
        g_capture_frame_w = w;
        g_capture_frame_h = h;
    }

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read_fbo);
    glGetIntegerv(GL_READ_BUFFER, &saved_read_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &saved_pack_alignment);
    saved_scissor = glIsEnabled(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK); /* default-framebuffer back buffer, pre-swap: defined in GLES3 */
    glDisable(GL_SCISSOR_TEST);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, g_capture_frame_buf);
    GLenum err = glGetError();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved_read_fbo);
    glReadBuffer((GLenum)saved_read_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, saved_pack_alignment);
    if (saved_scissor) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }

    g_capture_frame_valid = (err == GL_NO_ERROR);
}

/* Return the most recently stashed default-framebuffer capture (bottom-up RGB,
 * GL convention — same as glReadPixels/the front-buffer path, so downstream BMP
 * handling is unchanged). Returns false if no valid frame is stashed. */
bool gfx_opengl_get_captured_frame(int *out_w, int *out_h, const uint8_t **out_px) {
    if (!g_capture_frame_valid || g_capture_frame_buf == NULL) {
        return false;
    }
    if (out_w) *out_w = g_capture_frame_w;
    if (out_h) *out_h = g_capture_frame_h;
    if (out_px) *out_px = g_capture_frame_buf;
    return true;
}

bool gfx_opengl_copy_captured_frame(
    int width, int height, uint8_t *rgb_out) {
    size_t bytes;

    if (!g_capture_frame_valid || g_capture_frame_buf == NULL ||
        rgb_out == NULL || width <= 0 || height <= 0 ||
        width != g_capture_frame_w || height != g_capture_frame_h) {
        return false;
    }
    if ((size_t)width > SIZE_MAX / 3U / (size_t)height) {
        return false;
    }
    bytes = (size_t)width * (size_t)height * 3U;
    memcpy(rgb_out, g_capture_frame_buf, bytes);
    return true;
}

/* AUDIT-0040 verification harness (GE007_SYNTH_FRAME_PATTERN): overwrite the
 * whole default framebuffer with a solid color that deterministically encodes a
 * per-presented-frame counter (R=(n>>16)&255, G=(n>>8)&255, B=n&255). Drawn on
 * the GL/GLES path after composition so every capture route (desktop GL front
 * buffer, GLES stash above) reads it, letting a ROM-free harness prove the
 * capture reflects THIS frame and not a stale one. Env-gated by the caller;
 * inert (never invoked) by default. */
void gfx_opengl_draw_synth_frame(unsigned int n) {
    GLboolean saved_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat r = (GLfloat)((n >> 16) & 0xFFu) / 255.0f;
    GLfloat g = (GLfloat)((n >> 8) & 0xFFu) / 255.0f;
    GLfloat b = (GLfloat)(n & 0xFFu) / 255.0f;

    /* Fill the composited scene region of the default framebuffer with the
     * counter color. The final present may letterbox the scene inside the
     * drawable (as it does for the real game frame — the bars are genuine
     * presented pixels, not a capture artifact), so the harness decodes from the
     * scene region rather than assuming a full-drawable fill. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (saved_scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
}

/* Largest square offscreen dimension this GL context can allocate.  The scene
 * color attachment is a TEXTURE (GL_MAX_TEXTURE_SIZE) while the depth and MSAA
 * attachments are RENDERBUFFERS (GL_MAX_RENDERBUFFER_SIZE); they share one FBO,
 * so the safe limit is the MIN of the two.  Queried once (lazy) like the
 * anisotropy/MSAA limits above.  Exposed to gfx_pc.c so the scaled scene
 * dimensions can be clamped at their single producer (keeps aspect_ratio and
 * gfx_current_dimensions consistent — clamping inside ensure_scene_target would
 * crop the viewport instead). */
int gfx_opengl_max_offscreen_dim(void) {
    static int max_dim = -1;
    if (max_dim < 0) {
        GLint max_tex = 0, max_rb = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
        int m = (int)max_tex;
        if ((int)max_rb > 0 && (int)max_rb < m) {
            m = (int)max_rb;
        }
        max_dim = m > 0 ? m : 2048; /* conservative GL3.3 floor if the driver lies */
    }
    return max_dim;
}

static float gfx_opengl_effective_render_scale(void) {
    /* Same NaN hole as gfx_clamped_render_scale() in gfx_pc.c: NaN compares
     * false against both bounds below and would otherwise pass through
     * untouched into scene-target sizing math. Clamp to the floor. */
    if (g_pcRenderScale != g_pcRenderScale) {
        return 1.0f;
    }
    if (g_pcRenderScale < 1.0f) {
        return 1.0f;
    }
    if (g_pcRenderScale > 4.0f) {
        return 4.0f;
    }
    return g_pcRenderScale;
}

static int gfx_opengl_effective_msaa_samples(void) {
    static int max_samples = -1;
    static int last_requested = -1;
    static int last_effective = -1;
    static int warned_clamp;
    int requested = g_pcMsaaSamples;
    int effective = 0;

    if (requested < 2) {
        return 0;
    }

    if (max_samples < 0) {
        GLint gl_max_samples = 0;

        glGetIntegerv(GL_MAX_SAMPLES, &gl_max_samples);
        max_samples = gl_max_samples > 0 ? (int)gl_max_samples : 0;
    }

    if (requested >= 8 && max_samples >= 8) {
        effective = 8;
    } else if (requested >= 4 && max_samples >= 4) {
        effective = 4;
    } else if (requested >= 2 && max_samples >= 2) {
        effective = 2;
    }

    if ((requested != last_requested || effective != last_effective) &&
        requested > 0 && effective != requested && !warned_clamp) {
        fprintf(stderr,
                "[fast3d] Video.MSAA=%d clamped to %d (GL_MAX_SAMPLES=%d)\n",
                requested, effective, max_samples);
        fflush(stderr);
        warned_clamp = 1;
    }
    last_requested = requested;
    last_effective = effective;

    return effective;
}

/* SSAO is a remaster screen-space effect: gated by the master RemasterFX switch
 * plus its own Video.Ssao key. When active it needs the sampleable scene depth
 * texture, which only exists when the scene renders to the FBO (below). */
static bool gfx_opengl_output_ssao_active(void) {
    if (!(g_pcRemasterFX && g_pcSsao != 0)) {
        return false;
    }
    /* SSAO v2 (hemisphere) is Metal-only: the per-pixel view-space reconstruction
     * math op-hangs Apple's GL-over-Metal translator (docs/design/METAL_BACKEND_PLAN.md:39)
     * and no non-Apple GL test box exists yet to validate a GLSL port (W3.E2.T6).
     * On GL, hemisphere mode logs ONCE and renders the planar v1 shader unchanged —
     * an explicit, not silent, fallback. No GL shader lines change. */
    if (g_pcSsaoMode == 2) {
        static int warned_ssao_mode;
        if (!warned_ssao_mode) {
            fprintf(stderr, "[fast3d] Video.SsaoMode=hemisphere is a Metal-only effect; "
                            "GL falls back to planar SSAO v1.\n");
            fflush(stderr);
            warned_ssao_mode = 1;
        }
    }
    /* Depth is only resolved to the sampleable single-sample texture on the
     * non-MSAA path (g_scene_depth_valid = !multisampled). Under MSAA, SSAO
     * silently no-ops — warn once so it is not a mystery, and keep it off. */
    if (gfx_opengl_effective_msaa_samples() > 0) {
        static int warned_ssao_msaa;
        if (!warned_ssao_msaa) {
            fprintf(stderr, "[fast3d] SSAO disabled while Video.MSAA>0 (scene depth is "
                            "not resolved from the multisample buffer); use RenderScale "
                            "for anti-aliasing, or set Video.MSAA=0.\n");
            fflush(stderr);
            warned_ssao_msaa = 1;
        }
        return false;
    }
    return true;
}

static bool gfx_opengl_scene_target_enabled(void) {
    float render_scale = gfx_opengl_effective_render_scale();
    return g_pcRemasterFX ||
           render_scale > 1.001f ||
           gfx_opengl_effective_msaa_samples() > 0 ||
           gfx_opengl_output_ssao_active() ||   /* force scene FBO so depth is sampleable */
           gfx_diag_xlu_coverage_stencil_enabled() ||
           gfx_diag_xlu_rdp_memory_blend_enabled() ||
           gfx_diag_xlu_rdp_cvg_memory_blend_enabled() ||
           gfx_opengl_room_xlu_cvg_memory_enabled();
}

static bool gfx_opengl_ensure_scene_target(int width, int height) {
    GLenum status;
    int samples = gfx_opengl_effective_msaa_samples();
    bool need_stencil = gfx_diag_xlu_coverage_stencil_enabled();
    GLint saved_active_texture = 0;
    GLint saved_texture0 = 0;

    if (width <= 0 || height <= 0) {
        return false;
    }

    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture0);

    /* FBO setup binds its color texture on unit 0. The Fast3D layer caches
     * texture bindings separately, so restore the raw GL binding before any
     * queued triangles flush against stale cached state. */
#define RESTORE_SCENE_TEXTURE_BINDING() do { \
        glBindTexture(GL_TEXTURE_2D, (GLuint)saved_texture0); \
        glActiveTexture((GLenum)saved_active_texture); \
    } while (0)

    if (g_scene_fbo == 0) {
        glGenFramebuffers(1, &g_scene_fbo);
    }
    if (g_scene_color_tex == 0) {
        glGenTextures(1, &g_scene_color_tex);
    }
    if (g_scene_depth_tex == 0) {
        glGenTextures(1, &g_scene_depth_tex);
    }

    glBindTexture(GL_TEXTURE_2D, g_scene_color_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    if (g_scene_w != width || g_scene_h != height ||
        g_scene_has_stencil != need_stencil) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        /* Depth as a sampleable texture (single-sample) so the output pass can
         * read it for SSAO (§4 T1.1). NEAREST + clamp: depth must not filter. */
        glBindTexture(GL_TEXTURE_2D, g_scene_depth_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        if (need_stencil) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0,
                         GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                         GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        }
        g_scene_w = width;
        g_scene_h = height;
        g_scene_has_stencil = need_stencil;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           g_scene_color_tex, 0);
    if (need_stencil) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, g_scene_depth_tex, 0);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, g_scene_depth_tex, 0);
    }
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        static int warned;

        if (!warned) {
            fprintf(stderr,
                    "[fast3d] Scene render target incomplete: 0x%04X\n",
                    (unsigned int)status);
            fflush(stderr);
            warned = 1;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        RESTORE_SCENE_TEXTURE_BINDING();
        return false;
    }

    if (samples > 0) {
        if (g_scene_msaa_fbo == 0) {
            glGenFramebuffers(1, &g_scene_msaa_fbo);
        }
        if (g_scene_msaa_color_rb == 0) {
            glGenRenderbuffers(1, &g_scene_msaa_color_rb);
        }
        if (g_scene_msaa_depth_rb == 0) {
            glGenRenderbuffers(1, &g_scene_msaa_depth_rb);
        }

        if (g_scene_msaa_w != width ||
            g_scene_msaa_h != height ||
            g_scene_msaa_samples != samples ||
            g_scene_msaa_has_stencil != need_stencil) {
            glBindRenderbuffer(GL_RENDERBUFFER, g_scene_msaa_color_rb);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8,
                                             width, height);
            glBindRenderbuffer(GL_RENDERBUFFER, g_scene_msaa_depth_rb);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                             need_stencil ? GL_DEPTH24_STENCIL8 :
                                             GL_DEPTH_COMPONENT24,
                                             width, height);
            g_scene_msaa_w = width;
            g_scene_msaa_h = height;
            g_scene_msaa_samples = samples;
            g_scene_msaa_has_stencil = need_stencil;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, g_scene_msaa_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, g_scene_msaa_color_rb);
        if (need_stencil) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                      GL_RENDERBUFFER, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, g_scene_msaa_depth_rb);
        } else {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                      GL_RENDERBUFFER, g_scene_msaa_depth_rb);
        }
        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            static int warned_msaa;

            if (!warned_msaa) {
                fprintf(stderr,
                        "[fast3d] MSAA scene render target incomplete: 0x%04X "
                        "(samples=%d)\n",
                        (unsigned int)status, samples);
                fflush(stderr);
                warned_msaa = 1;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            RESTORE_SCENE_TEXTURE_BINDING();
            return false;
        }
    }

    RESTORE_SCENE_TEXTURE_BINDING();
#undef RESTORE_SCENE_TEXTURE_BINDING

    return true;
}

static bool gfx_opengl_diag_output_vi_filter(void) {
    if (!g_diag_output_vi_filter_checked) {
        const char *env = getenv("GE007_DIAG_OUTPUT_VI_FILTER");
        int w = 0;
        int h = 0;

        g_diag_output_vi_filter_checked = 1;
        if (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) {
            if (sscanf(env, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                g_diag_output_vi_filter_w = w;
                g_diag_output_vi_filter_h = h;
                g_diag_output_vi_filter_enabled = 1;
                fprintf(stderr,
                        "[fast3d] DIAG OUTPUT VI FILTER ENABLED %dx%d "
                        "(GE007_DIAG_OUTPUT_VI_FILTER)\n",
                        w, h);
                fflush(stderr);
            } else {
                fprintf(stderr,
                        "[fast3d] Ignoring invalid GE007_DIAG_OUTPUT_VI_FILTER=%s "
                        "(expected WxH)\n",
                        env);
                fflush(stderr);
            }
        } else if (env != NULL && strcmp(env, "0") == 0) {
            g_diag_output_vi_filter_disabled = 1;
        }
    }

    return g_diag_output_vi_filter_enabled != 0;
}

static bool gfx_opengl_env_flag_enabled(const char *name) {
    const char *env = getenv(name);
    return env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
}

static bool gfx_opengl_current_menu_uses_auto_vi_filter(void) {
#ifdef NATIVE_PORT
    /* mdkr64: GoldenEye's paper-menu auto-VI-filter heuristic keys off GE's
     * `current_menu` global, which DKR does not have. The DKR port does not use
     * this post-FX path, so it is always off. */
    return false;
#else
    switch (current_menu) {
        case MENU_FILE_SELECT:
        case MENU_MODE_SELECT:
        case MENU_MISSION_SELECT:
        case MENU_DIFFICULTY:
        case MENU_007_OPTIONS:
        case MENU_BRIEFING:
        case MENU_MISSION_FAILED:
        case MENU_MISSION_COMPLETE:
        case MENU_MP_OPTIONS:
        case MENU_MP_CHAR_SELECT:
        case MENU_MP_HANDICAP:
        case MENU_MP_CONTROL_STYLE:
        case MENU_MP_STAGE_SELECT:
        case MENU_MP_SCENARIO_SELECT:
        case MENU_MP_TEAMS:
        case MENU_CHEAT:
        case MENU_NO_CONTROLLERS:
            return true;
        default:
            return false;
    }
#endif
}

static bool gfx_opengl_auto_menu_vi_filter(int *filter_w, int *filter_h) {
    if (!g_auto_menu_vi_filter_checked) {
        g_auto_menu_vi_filter_disabled =
            gfx_opengl_env_flag_enabled("GE007_DISABLE_AUTO_MENU_VI_FILTER") ? 1 : 0;
        if (g_auto_menu_vi_filter_disabled) {
            fprintf(stderr,
                    "[fast3d] Auto menu output VI filter disabled "
                    "(GE007_DISABLE_AUTO_MENU_VI_FILTER)\n");
            fflush(stderr);
        }
        g_auto_menu_vi_filter_checked = 1;
    }

    if (g_pcRetroFilterMode == PC_RETRO_FILTER_OFF ||
        g_auto_menu_vi_filter_disabled ||
        !gfx_opengl_current_menu_uses_auto_vi_filter() ||
        viGetX() != 440 ||
        viGetY() != 330) {
        return false;
    }

    if (!g_auto_menu_vi_filter_logged) {
        fprintf(stderr,
                "[fast3d] Auto paper-menu output VI filter enabled 440x330 "
                "(logical VI 440x330; set GE007_DISABLE_AUTO_MENU_VI_FILTER=1 to disable)\n");
        fflush(stderr);
        g_auto_menu_vi_filter_logged = 1;
    }

    if (filter_w != NULL) {
        *filter_w = 440;
    }
    if (filter_h != NULL) {
        *filter_h = 330;
    }
    return true;
}

static bool gfx_opengl_current_frame_uses_auto_gameplay_vi_filter(void) {
#ifdef NATIVE_PORT
    /* mdkr64: GE gameplay auto-VI-filter heuristic keys off GE's current_menu /
     * gamemode globals. Not used by the DKR port. */
    return false;
#else
    if (current_menu == MENU_RUN_STAGE || current_menu == MENU_DISPLAY_CAST) {
        return true;
    }

    return current_menu == MENU_INVALID &&
           (gamemode == GAMEMODE_SOLO || gamemode == GAMEMODE_MULTI) &&
           viGetY() <= 240;
#endif
}

static bool gfx_opengl_auto_gameplay_vi_filter(int framebuffer_w,
                                               int framebuffer_h,
                                               int *filter_w,
                                               int *filter_h) {
    const int target_h = 240;
    int target_w;
    bool setting_enabled = g_pcRetroFilterMode == PC_RETRO_FILTER_ON;
    bool setting_disabled = g_pcRetroFilterMode == PC_RETRO_FILTER_OFF;

    if (!g_auto_gameplay_vi_filter_checked) {
        /* The N64 VI filter bilinearly downsamples to 240p then upsamples, which
         * softens the entire image. Default it OFF for a sharp picture; users who
         * want the more hardware-accurate (softer) look can opt in. The explicit
         * DISABLE flag is kept so existing configs/scripts keep working. */
        g_auto_gameplay_vi_filter_enabled =
            gfx_opengl_env_flag_enabled("GE007_ENABLE_AUTO_GAMEPLAY_VI_FILTER") ? 1 : 0;
        g_auto_gameplay_vi_filter_disabled =
            gfx_opengl_env_flag_enabled("GE007_DISABLE_AUTO_GAMEPLAY_VI_FILTER") ? 1 : 0;
        if ((setting_enabled || g_auto_gameplay_vi_filter_enabled) &&
            !setting_disabled &&
            !g_auto_gameplay_vi_filter_disabled) {
            fprintf(stderr,
                    "[fast3d] Auto gameplay/display-cast output VI filter enabled "
                    "(Video.RetroFilter or GE007_ENABLE_AUTO_GAMEPLAY_VI_FILTER)\n");
            fflush(stderr);
        }
        g_auto_gameplay_vi_filter_checked = 1;
    }

    if (setting_disabled ||
        (!setting_enabled && !g_auto_gameplay_vi_filter_enabled) ||
        g_auto_gameplay_vi_filter_disabled ||
        framebuffer_w <= 0 ||
        framebuffer_h < target_h) {
        return false;
    }

    if (!gfx_opengl_current_frame_uses_auto_gameplay_vi_filter()) {
        return false;
    }

    target_w = (framebuffer_w * target_h) / framebuffer_h;
    if (target_w < 1) {
        target_w = 1;
    }
    if ((target_w & 1) != 0 && target_w > 1) {
        target_w--;
    }

    if (!g_auto_gameplay_vi_filter_logged) {
        fprintf(stderr,
                "[fast3d] Auto gameplay/display-cast output VI filter target %dx%d "
                "(framebuffer %dx%d; set GE007_DISABLE_AUTO_GAMEPLAY_VI_FILTER=1 to disable)\n",
                target_w, target_h, framebuffer_w, framebuffer_h);
        fflush(stderr);
        g_auto_gameplay_vi_filter_logged = 1;
    }

    if (filter_w != NULL) {
        *filter_w = target_w;
    }
    if (filter_h != NULL) {
        *filter_h = target_h;
    }
    return true;
}

static bool gfx_opengl_output_vi_filter_target(int framebuffer_w,
                                               int framebuffer_h,
                                               int *filter_w,
                                               int *filter_h) {
    if (gfx_opengl_diag_output_vi_filter()) {
        if (filter_w != NULL) {
            *filter_w = g_diag_output_vi_filter_w;
        }
        if (filter_h != NULL) {
            *filter_h = g_diag_output_vi_filter_h;
        }
        return true;
    }

    if (g_diag_output_vi_filter_disabled) {
        return false;
    }

    if (gfx_opengl_auto_menu_vi_filter(filter_w, filter_h)) {
        return true;
    }

    return gfx_opengl_auto_gameplay_vi_filter(framebuffer_w, framebuffer_h,
                                              filter_w, filter_h);
}

static bool gfx_opengl_diag_output_vi_logical_size(int *width, int *height) {
    if (!g_diag_output_vi_logical_checked) {
        const char *env = getenv("GE007_DIAG_OUTPUT_VI_LOGICAL_SIZE");
        int w = 0;
        int h = 0;

        g_diag_output_vi_logical_checked = 1;
        if (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) {
            if (sscanf(env, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                g_diag_output_vi_logical_w = w;
                g_diag_output_vi_logical_h = h;
                g_diag_output_vi_logical_enabled = 1;
                fprintf(stderr,
                        "[fast3d] DIAG OUTPUT VI LOGICAL SIZE ENABLED %dx%d "
                        "(GE007_DIAG_OUTPUT_VI_LOGICAL_SIZE)\n",
                        w, h);
                fflush(stderr);
            } else {
                fprintf(stderr,
                        "[fast3d] Ignoring invalid GE007_DIAG_OUTPUT_VI_LOGICAL_SIZE=%s "
                        "(expected WxH)\n",
                        env);
                fflush(stderr);
            }
        }
    }

    if (g_diag_output_vi_logical_enabled) {
        if (width != NULL) {
            *width = g_diag_output_vi_logical_w;
        }
        if (height != NULL) {
            *height = g_diag_output_vi_logical_h;
        }
        return true;
    }

    return false;
}

static void gfx_opengl_check_output_filter_color_diag(void) {
    if (!g_diag_output_filter_color_checked) {
        const char *scale_env = getenv("GE007_DIAG_OUTPUT_COLOR_SCALE");
        const char *bias_env = getenv("GE007_DIAG_OUTPUT_COLOR_BIAS");

        g_diag_output_filter_color_checked = 1;
        if (scale_env != NULL && scale_env[0] != '\0') {
            g_diag_output_filter_color_scale = (float)atof(scale_env);
        }
        if (bias_env != NULL && bias_env[0] != '\0') {
            g_diag_output_filter_color_bias = (float)atof(bias_env);
        }
        if (g_diag_output_filter_color_scale != 1.0f ||
            g_diag_output_filter_color_bias != 0.0f) {
            fprintf(stderr,
                    "[fast3d] DIAG OUTPUT COLOR scale=%.6f bias=%.6f "
                    "(GE007_DIAG_OUTPUT_COLOR_SCALE/BIAS)\n",
                    g_diag_output_filter_color_scale,
                    g_diag_output_filter_color_bias);
            fflush(stderr);
        }
    }
}

static int gfx_opengl_diag_output_rgb555_mode(void) {
    if (!g_diag_output_rgb555_checked) {
        const char *env = getenv("GE007_DIAG_OUTPUT_RGB555");

        if (env != NULL && env[0] != '\0') {
            if (strcmp(env, "1") == 0 ||
                strcmp(env, "on") == 0 ||
                strcmp(env, "true") == 0) {
                g_diag_output_rgb555_mode = 1;
            } else if (strcmp(env, "dither") == 0) {
                g_diag_output_rgb555_mode = 2;
            } else if (strcmp(env, "0") != 0 && strcmp(env, "off") != 0) {
                fprintf(stderr,
                        "[fast3d] Ignoring invalid GE007_DIAG_OUTPUT_RGB555=%s "
                        "(expected 1, on, true, dither, 0, or off)\n",
                        env);
                fflush(stderr);
            }
        }
        if (g_diag_output_rgb555_mode) {
            fprintf(stderr,
                    "[fast3d] DIAG output RGB555 mode=%s "
                    "(GE007_DIAG_OUTPUT_RGB555)\n",
                    g_diag_output_rgb555_mode == 2 ? "dither" : "quantize");
            fflush(stderr);
        }
        g_diag_output_rgb555_checked = 1;
    }

    return g_diag_output_rgb555_mode;
}

static float gfx_opengl_output_gamma(void) {
    if (g_pcVideoGamma < 0.5f) {
        return 0.5f;
    }
    if (g_pcVideoGamma > 2.5f) {
        return 2.5f;
    }
    return g_pcVideoGamma;
}

static float gfx_opengl_output_saturation(void) {
    float v = g_pcVideoSaturation;
    return v < 0.0f ? 0.0f : (v > 2.0f ? 2.0f : v);
}

static float gfx_opengl_output_contrast(void) {
    float v = g_pcVideoContrast;
    return v < 0.5f ? 0.5f : (v > 2.0f ? 2.0f : v);
}

static float gfx_opengl_output_brightness(void) {
    float v = g_pcVideoBrightness;
    return v < -0.5f ? -0.5f : (v > 0.5f ? 0.5f : v);
}

static float gfx_opengl_output_vignette(void) {
    float v = g_pcVignette;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float gfx_opengl_output_sharpen(void) {
    float v = g_pcSharpen;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static bool gfx_opengl_output_color_adjust_active(void) {
    float gamma = gfx_opengl_output_gamma();

    /* Gamma + the diag color knobs are always honored (display/diag, not remaster). */
    if (g_diag_output_filter_color_scale != 1.0f ||
        g_diag_output_filter_color_bias != 0.0f ||
        gfx_opengl_diag_output_rgb555_mode() != 0 ||
        gamma < 0.999f || gamma > 1.001f) {
        return true;
    }
    /* Master faithful switch: when Video.RemasterFX is off, NONE of the remaster
     * post-FX run, so the output pass is a pure passthrough (faithful original
     * look) regardless of the individual grade/tonemap/etc. settings. */
    if (!g_pcRemasterFX) {
        return false;
    }
    return gfx_opengl_output_saturation() != 1.0f ||
           gfx_opengl_output_contrast() != 1.0f ||
           gfx_opengl_output_brightness() != 0.0f ||
           g_pcOutputDither != 0 ||
           gfx_opengl_output_vignette() > 0.0001f ||
           g_pcFxaa != 0 ||
           gfx_opengl_output_sharpen() > 0.0001f ||
           (g_pcTonemap != 0 && g_pcGradeLevelValid) ||
           (g_pcGradePresets && g_pcGradeLevelValid &&
                                (g_pcGradeLevelSat != 1.0f ||
                                 g_pcGradeLevelCon != 1.0f ||
                                 g_pcGradeLevelTintR != 1.0f ||
                                 g_pcGradeLevelTintG != 1.0f ||
                                 g_pcGradeLevelTintB != 1.0f));
}

static GLuint gfx_opengl_compile_filter_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    GLint success = GL_FALSE;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success != GL_TRUE) {
        char error_log[1024];
        GLsizei length = 0;

        glGetShaderInfoLog(shader, sizeof(error_log), &length, error_log);
        fprintf(stderr,
                "[fast3d] Output VI filter shader compile failed: %.*s\n",
                (int)length, error_log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static bool gfx_opengl_ensure_output_filter_program(void) {
    static const char *vs_source =
#ifdef MGB64_PORTMASTER_GLES
        "#version 320 es\n"
        "precision mediump float;\n"
#else
        "#version 330 core\n"
#endif
        "out vec2 vTexCoord;\n"
        "const vec2 kPos[3] = vec2[3](\n"
        "    vec2(-1.0, -1.0),\n"
        "    vec2( 3.0, -1.0),\n"
        "    vec2(-1.0,  3.0));\n"
        "void main() {\n"
        "    vec2 pos = kPos[gl_VertexID];\n"
        "    vTexCoord = pos * 0.5 + 0.5;\n"
        "    gl_Position = vec4(pos, 0.0, 1.0);\n"
        "}\n";
    static const char *fs_source =
#ifdef MGB64_PORTMASTER_GLES
        "#version 320 es\n"
        "precision mediump float;\n"
#else
        "#version 330 core\n"
#endif
        "uniform sampler2D uTex;\n"
        "uniform vec2 uSrcSize;\n"
        "uniform vec2 uDstSize;\n"
        "uniform float uColorScale;\n"
        "uniform float uColorBias;\n"
        "uniform float uGamma;\n"
        "uniform float uSaturation;\n"
        "uniform float uContrast;\n"
        "uniform float uBrightness;\n"
        "uniform int uApplyPost;\n"
        "uniform int uDither;\n"
        "uniform float uVignette;\n"
        "uniform int uBloom;\n"
        "uniform float uBloomThreshold;\n"
        "uniform float uBloomIntensity;\n"
        "uniform sampler2D uDepthTex;\n"
        "uniform int uSsao;\n"
        "uniform float uSsaoRadius;\n"
        "uniform float uSsaoIntensity;\n"
        "uniform float uSsaoAspect;\n"
        "uniform float uSsaoProjA;\n"
        "uniform float uSsaoProjB;\n"
        "uniform int uFilterMode;\n"
        "uniform int uFxaa;\n"
        "uniform float uSharpen;\n"
        "uniform float uLevelSat;\n"
        "uniform float uLevelCon;\n"
        "uniform vec3 uColorTint;\n"
        "uniform int uTonemap;\n"
        "uniform int uLinearFinish;\n"
        "uniform int uRgb555;\n"
        "const float kBayer4[16] = float[16](\n"
        "    0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,\n"
        "   12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,\n"
        "    3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,\n"
        "   15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0);\n"
        "in vec2 vTexCoord;\n"
        "out vec4 outColor;\n"
        "vec4 sampleNearest(vec2 dstCoord) {\n"
        "    ivec2 p = ivec2(floor(dstCoord * uSrcSize / uDstSize));\n"
        "    p = clamp(p, ivec2(0), ivec2(uSrcSize) - ivec2(1));\n"
        "    return texelFetch(uTex, p, 0);\n"
        "}\n"
        "vec2 fitSizeForAspect(vec2 boundsSize, float aspect) {\n"
        "    float boundsAspect = boundsSize.x / boundsSize.y;\n"
        "    if (boundsAspect > aspect) {\n"
        "        return vec2(boundsSize.y * aspect, boundsSize.y);\n"
        "    }\n"
        "    return vec2(boundsSize.x, boundsSize.x / aspect);\n"
        "}\n"
        "vec4 sampleFitSrcToDstNearest(vec2 dstCoord) {\n"
        "    float srcAspect = uSrcSize.x / uSrcSize.y;\n"
        "    vec2 fitSize = fitSizeForAspect(uDstSize, srcAspect);\n"
        "    vec2 offset = floor((uDstSize - fitSize) * 0.5);\n"
        "    if (dstCoord.x < offset.x || dstCoord.y < offset.y ||\n"
        "        dstCoord.x >= offset.x + fitSize.x || dstCoord.y >= offset.y + fitSize.y) {\n"
        "        return vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    }\n"
        "    ivec2 p = ivec2(floor((dstCoord - offset) * uSrcSize / fitSize));\n"
        "    p = clamp(p, ivec2(0), ivec2(uSrcSize) - ivec2(1));\n"
        "    return texelFetch(uTex, p, 0);\n"
        "}\n"
        "vec4 sampleFitLogicalToDstNearest(vec2 dstCoord) {\n"
        "    float dstAspect = uDstSize.x / uDstSize.y;\n"
        "    vec2 fitSize = fitSizeForAspect(uSrcSize, dstAspect);\n"
        "    vec2 offset = floor((uSrcSize - fitSize) * 0.5);\n"
        "    ivec2 p = ivec2(floor(offset + dstCoord * fitSize / uDstSize));\n"
        "    p = clamp(p, ivec2(0), ivec2(uSrcSize) - ivec2(1));\n"
        "    return texelFetch(uTex, p, 0);\n"
        "}\n"
        "vec4 sampleCpuBilinear(vec2 dstCoord) {\n"
        "    vec2 srcCoord = dstCoord * uSrcSize / uDstSize - vec2(0.5);\n"
        "    ivec2 p0 = ivec2(floor(srcCoord));\n"
        "    vec2 f = srcCoord - vec2(p0);\n"
        "    if (p0.x < 0) { p0.x = 0; f.x = 0.0; }\n"
        "    else if (p0.x >= int(uSrcSize.x) - 1) { p0.x = int(uSrcSize.x) - 1; f.x = 0.0; }\n"
        "    if (p0.y < 0) { p0.y = 0; f.y = 0.0; }\n"
        "    else if (p0.y >= int(uSrcSize.y) - 1) { p0.y = int(uSrcSize.y) - 1; f.y = 0.0; }\n"
        "    ivec2 p1 = min(p0 + ivec2(1), ivec2(uSrcSize) - ivec2(1));\n"
        "    vec4 c00 = texelFetch(uTex, p0, 0);\n"
        "    vec4 c10 = texelFetch(uTex, ivec2(p1.x, p0.y), 0);\n"
        "    vec4 c01 = texelFetch(uTex, ivec2(p0.x, p1.y), 0);\n"
        "    vec4 c11 = texelFetch(uTex, p1, 0);\n"
        "    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);\n"
        "}\n"
        "vec4 sampleDst(vec2 dstCoord) {\n"
        "    if (uFilterMode == 1) return sampleNearest(dstCoord);\n"
        "    else if (uFilterMode == 2) return sampleFitSrcToDstNearest(dstCoord);\n"
        "    else if (uFilterMode == 3) return sampleFitLogicalToDstNearest(dstCoord);\n"
        "    return sampleCpuBilinear(dstCoord);\n"
        "}\n"
        "float fxLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
        "vec3 srgbToLinear(vec3 c) {\n"
        "    c = clamp(c, 0.0, 1.0);\n"
        "    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)),\n"
        "               step(vec3(0.04045), c));\n"
        "}\n"
        "vec3 linearToSrgb(vec3 c) {\n"
        "    c = max(c, 0.0);\n"
        "    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,\n"
        "               step(vec3(0.0031308), c));\n"
        "}\n"
        "vec3 filmicFinish(vec3 c) {\n"
        "    /* White 1.0 is fixed exactly. The 0.18 shoulder maps linear\n"
        "     * mid-grey 0.18 -> 0.2057; the 20% blend yields 0.1851. */\n"
        "    vec3 shoulder = c * 1.18 / (vec3(1.0) + c * 0.18);\n"
        "    return mix(c, shoulder, 0.20);\n"
        "}\n"
        "vec3 fxaa(vec2 fc, vec3 rgbM) {\n"
        "    float lM = fxLuma(rgbM);\n"
        "    float lN = fxLuma(sampleDst(fc + vec2( 0.0,-1.0)).rgb);\n"
        "    float lS = fxLuma(sampleDst(fc + vec2( 0.0, 1.0)).rgb);\n"
        "    float lW = fxLuma(sampleDst(fc + vec2(-1.0, 0.0)).rgb);\n"
        "    float lE = fxLuma(sampleDst(fc + vec2( 1.0, 0.0)).rgb);\n"
        "    float lNW = fxLuma(sampleDst(fc + vec2(-1.0,-1.0)).rgb);\n"
        "    float lNE = fxLuma(sampleDst(fc + vec2( 1.0,-1.0)).rgb);\n"
        "    float lSW = fxLuma(sampleDst(fc + vec2(-1.0, 1.0)).rgb);\n"
        "    float lSE = fxLuma(sampleDst(fc + vec2( 1.0, 1.0)).rgb);\n"
        "    float lMin = min(lM, min(min(lN,lS), min(lW,lE)));\n"
        "    float lMax = max(lM, max(max(lN,lS), max(lW,lE)));\n"
        "    float range = lMax - lMin;\n"
        "    if (range < max(0.0625, lMax * 0.125)) return rgbM;\n"
        "    vec2 dir;\n"
        "    dir.x = -((lNW + lNE) - (lSW + lSE));\n"
        "    dir.y =  ((lNW + lSW) - (lNE + lSE));\n"
        "    float dirReduce = max((lNW+lNE+lSW+lSE) * 0.03125, 0.0078125);\n"
        "    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
        "    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0));\n"
        "    vec3 rgbA = 0.5 * (\n"
        "        sampleDst(fc + dir * (1.0/3.0 - 0.5)).rgb +\n"
        "        sampleDst(fc + dir * (2.0/3.0 - 0.5)).rgb);\n"
        "    vec3 rgbB = rgbA * 0.5 + 0.25 * (\n"
        "        sampleDst(fc + dir * -0.5).rgb +\n"
        "        sampleDst(fc + dir *  0.5).rgb);\n"
        "    float lB = fxLuma(rgbB);\n"
        "    if (lB < lMin || lB > lMax) return rgbA;\n"
        "    return rgbB;\n"
        "}\n"
        "vec3 casSharpen(vec2 fc, vec3 rgbC) {\n"
        "    vec3 n = sampleDst(fc + vec2( 0.0,-1.0)).rgb;\n"
        "    vec3 s = sampleDst(fc + vec2( 0.0, 1.0)).rgb;\n"
        "    vec3 w = sampleDst(fc + vec2(-1.0, 0.0)).rgb;\n"
        "    vec3 e = sampleDst(fc + vec2( 1.0, 0.0)).rgb;\n"
        "    vec3 mn = min(rgbC, min(min(n,s), min(w,e)));\n"
        "    vec3 mx = max(rgbC, max(max(n,s), max(w,e)));\n"
        "    vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx, 0.0001), 0.0, 1.0);\n"
        "    amp = sqrt(amp);\n"
        "    float peak = -0.125 - 0.075 * uSharpen;\n"
        "    vec3 wgt = amp * peak;\n"
        "    vec3 sum = rgbC + (n + s + w + e) * wgt;\n"
        "    vec3 rcpW = 1.0 / (1.0 + 4.0 * wgt);\n"
        "    vec3 outc = clamp(sum * rcpW, mn, mx);\n"
        "    return mix(rgbC, outc, clamp(uSharpen, 0.0, 1.0));\n"
        "}\n"
        /* Screen-space ambient occlusion from the scene depth texture. Works on
         * raw (non-linear) window depth: a neighbour that is nearer than the
         * centre by more than a small, depth-adaptive margin is a contact
         * occluder. The margin scales with (1-depth) so it tracks the depth
         * buffer's non-linear precision (tight far, loose near). Sky/far
         * (depth ~= 1) gets no AO. */
        "const vec2 kSsaoDir[8] = vec2[8](\n"
        "    vec2( 1.0, 0.0), vec2( 0.7071, 0.7071), vec2(0.0, 1.0), vec2(-0.7071, 0.7071),\n"
        "    vec2(-1.0, 0.0), vec2(-0.7071,-0.7071), vec2(0.0,-1.0), vec2( 0.7071,-0.7071));\n"
        /* view-space distance (positive) from window depth d */
        "float ssaoLinZ(float d) {\n"
        "    return uSsaoProjB / (uSsaoProjA + 2.0 * d - 1.0);\n"
        "}\n"
        "float ssaoAO(vec2 uv) {\n"
        "    float cd = texture(uDepthTex, uv).r;\n"
        "    if (cd >= 0.99999) return 0.0;\n"
        "    float cz = ssaoLinZ(cd);\n"                       /* centre distance */
        "    float occ = 0.0;\n"
        "    for (int i = 0; i < 8; ++i) {\n"
        "        vec2 dir = kSsaoDir[i];\n"
        "        dir.x /= max(uSsaoAspect, 0.001);\n"
        "        for (int s = 1; s <= 2; ++s) {\n"
        "            vec2 o = dir * uSsaoRadius * float(s);\n"
        "            float nz = ssaoLinZ(texture(uDepthTex, uv + o).r);\n"
        "            float diff = cz - nz;\n"                   /* >0: neighbour nearer */
        /* thresholds are fractions of the centre distance -> scale-invariant across
         * the scene. The higher floor (1.5%) skips gentle receding slopes (no
         * normals to reject self-occlusion), so the effect reads as contact
         * darkening in creases/corners rather than a flat wash; the ceiling (12%)
         * ignores silhouette gaps that would halo. */
        "            if (diff > cz * 0.015 && diff < cz * 0.12) occ += 1.0 / float(s);\n"
        "        }\n"
        "    }\n"
        "    return occ / 12.0;\n"
        "}\n"
        "void main() {\n"
        "    vec4 color = sampleDst(gl_FragCoord.xy);\n"
        "    if (uApplyPost == 1 && uFxaa == 1) {\n"
        "        color.rgb = fxaa(gl_FragCoord.xy, color.rgb);\n"
        "    }\n"
        "    vec3 rgb = clamp(color.rgb * uColorScale + vec3(uColorBias / 255.0), 0.0, 1.0);\n"
        "    if (uLinearFinish == 1) {\n"
        "        rgb = srgbToLinear(rgb);\n"
        "        if (uSsao == 1) {\n"
        "            float ao = 1.0 - uSsaoIntensity * ssaoAO(vTexCoord);\n"
        "            rgb *= clamp(ao, 0.0, 1.0);\n"
        "        }\n"
        "        if (uBloom == 1) {\n"
        "            vec2 texel = 1.0 / uSrcSize;\n"
        "            vec3 bloom = vec3(0.0);\n"
        "            float wsum = 0.0;\n"
        "            const int R = 3;\n"
        "            for (int y = -R; y <= R; ++y)\n"
        "            for (int x = -R; x <= R; ++x) {\n"
        "                vec2 o = vec2(float(x), float(y)) * texel * 2.0;\n"
        "                vec3 s = srgbToLinear(texture(uTex, vTexCoord + o).rgb);\n"
        "                float l = dot(s, vec3(0.2126, 0.7152, 0.0722));\n"
        "                float b = max(l - uBloomThreshold, 0.0) / max(1.0 - uBloomThreshold, 0.001);\n"
        "                float w = exp(-float(x*x + y*y) / 6.0);\n"
        "                bloom += s * b * w;\n"
        "                wsum += w;\n"
        "            }\n"
        "            rgb += bloom / max(wsum, 0.001) * uBloomIntensity;\n"
        "        }\n"
        "        rgb += vec3(uBrightness);\n"
        "        float con = uContrast * uLevelCon;\n"
        "        rgb = (rgb - 0.18) * con + 0.18;\n"
        "        float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));\n"
        "        float sat = uSaturation * uLevelSat;\n"
        "        rgb = mix(vec3(luma), rgb, sat);\n"
        "        rgb *= uColorTint;\n"
        "        if (uTonemap == 1) {\n"
        "            rgb = filmicFinish(max(rgb, 0.0));\n"
        "        }\n"
        "        rgb = linearToSrgb(clamp(rgb, 0.0, 1.0));\n"
        "    }\n"
        "    rgb = pow(rgb, vec3(1.0 / max(uGamma, 0.001)));\n"
        "    if (uApplyPost == 1 && uVignette > 0.0) {\n"
        "        vec2 vc = vTexCoord - vec2(0.5);\n"
        "        float d = dot(vc, vc) * 2.0;\n"
        "        float vig = 1.0 - uVignette * smoothstep(0.3, 1.0, d);\n"
        "        rgb *= vig;\n"
        "    }\n"
        "    if (uApplyPost == 1 && uSharpen > 0.0) {\n"
        "        rgb = casSharpen(gl_FragCoord.xy, rgb);\n"
        "    }\n"
        "    if (uApplyPost == 1 && uDither == 1) {\n"
        "        ivec2 dp = ivec2(gl_FragCoord.xy) & 3;\n"
        "        float t = kBayer4[dp.y * 4 + dp.x] - 0.5;\n"
        "        rgb += vec3(t / 255.0);\n"
        "        rgb = clamp(rgb, 0.0, 1.0);\n"
        "    }\n"
        "    if (uRgb555 != 0) {\n"
        "        float threshold = 0.5;\n"
        "        if (uRgb555 == 2) {\n"
        "            ivec2 dp = ivec2(gl_FragCoord.xy) & 3;\n"
        "            threshold += kBayer4[dp.y * 4 + dp.x] - 0.5;\n"
        "        }\n"
        "        rgb = floor(clamp(rgb, 0.0, 1.0) * 31.0 + threshold) / 31.0;\n"
        "        rgb = clamp(rgb, 0.0, 1.0);\n"
        "    }\n"
        "    outColor = vec4(rgb, color.a);\n"
        "}\n";

    if (g_output_filter_program == 0) {
        GLuint vs = gfx_opengl_compile_filter_shader(GL_VERTEX_SHADER, vs_source);
        GLuint fs = gfx_opengl_compile_filter_shader(GL_FRAGMENT_SHADER, fs_source);
        GLint success = GL_FALSE;

        if (vs == 0 || fs == 0) {
            glDeleteShader(vs);
            glDeleteShader(fs);
            return false;
        }

        g_output_filter_program = glCreateProgram();
        glAttachShader(g_output_filter_program, vs);
        glAttachShader(g_output_filter_program, fs);
        glLinkProgram(g_output_filter_program);
        glDeleteShader(vs);
        glDeleteShader(fs);

        glGetProgramiv(g_output_filter_program, GL_LINK_STATUS, &success);
        if (success != GL_TRUE) {
            char error_log[1024];
            GLsizei length = 0;

            glGetProgramInfoLog(g_output_filter_program, sizeof(error_log), &length, error_log);
            fprintf(stderr,
                    "[fast3d] Output VI filter program link failed: %.*s\n",
                    (int)length, error_log);
            glDeleteProgram(g_output_filter_program);
            g_output_filter_program = 0;
            return false;
        }

    }

    if (g_output_filter_vao == 0) {
        glGenVertexArrays(1, &g_output_filter_vao);
    }

    if (g_output_filter_fbo == 0) {
        glGenFramebuffers(1, &g_output_filter_fbo);
    }

    return g_output_filter_program != 0 && g_output_filter_vao != 0 && g_output_filter_fbo != 0;
}

static void gfx_opengl_ensure_filter_texture(GLuint *tex_id,
                                             int *tex_w,
                                             int *tex_h,
                                             int width,
                                             int height) {
    if (*tex_id == 0) {
        glGenTextures(1, tex_id);
    }

    glBindTexture(GL_TEXTURE_2D, *tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    if (*tex_w != width || *tex_h != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        *tex_w = width;
        *tex_h = height;
    }
}

static void gfx_opengl_draw_output_filter_texture(GLuint texture_id,
                                                  int src_w, int src_h,
                                                  int dst_w, int dst_h,
                                                  int filter_mode,
                                                  int apply_post) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glUseProgram(g_output_filter_program);
    glUniform1i(glGetUniformLocation(g_output_filter_program, "uTex"), 0);
    glUniform2f(glGetUniformLocation(g_output_filter_program, "uSrcSize"),
                (float)src_w, (float)src_h);
    glUniform2f(glGetUniformLocation(g_output_filter_program, "uDstSize"),
                (float)dst_w, (float)dst_h);
    glUniform1f(glGetUniformLocation(g_output_filter_program, "uColorScale"),
                g_diag_output_filter_color_scale);
    glUniform1f(glGetUniformLocation(g_output_filter_program, "uColorBias"),
                g_diag_output_filter_color_bias);
    glUniform1f(glGetUniformLocation(g_output_filter_program, "uGamma"),
                gfx_opengl_output_gamma());
    glUniform1f(glGetUniformLocation(g_output_filter_program, "uSaturation"),
                gfx_opengl_output_saturation());
    glUniform1f(glGetUniformLocation(g_output_filter_program, "uContrast"),
                gfx_opengl_output_contrast());
    glUniform1f(glGetUniformLocation(g_output_filter_program, "uBrightness"),
                gfx_opengl_output_brightness());
    glUniform1i(glGetUniformLocation(g_output_filter_program, "uApplyPost"),
                (apply_post && g_pcRemasterFX) ? 1 : 0);
    glUniform1i(glGetUniformLocation(g_output_filter_program, "uDither"),
                g_pcOutputDither ? 1 : 0);
    glUniform1f(glGetUniformLocation(g_output_filter_program, "uVignette"),
                gfx_opengl_output_vignette());
    glUniform1i(glGetUniformLocation(g_output_filter_program, "uBloom"),
                g_pcBloom ? 1 : 0);
    glUniform1f(glGetUniformLocation(g_output_filter_program, "uBloomThreshold"),
                g_pcBloomThreshold);
    glUniform1f(glGetUniformLocation(g_output_filter_program, "uBloomIntensity"),
                g_pcBloomIntensity);
    {
        /* g_pcRemasterFX gates SSAO (it is a remaster effect — see
         * gfx_opengl_output_ssao_active): required so SSAO does not apply with the
         * master remaster switch OFF when RenderScale>1 forces the scene FBO, and
         * to stay in lockstep with the Metal backend's SSAO gate. */
        int ssao_on = (apply_post && g_pcRemasterFX && g_pcSsao != 0 && g_scene_depth_valid &&
                       g_pc_ssao_proj_b != 0.0f) ? 1 : 0;
        if (ssao_on) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_scene_depth_tex);
            glActiveTexture(GL_TEXTURE0);
        }
        glUniform1i(glGetUniformLocation(g_output_filter_program, "uDepthTex"), 1);
        glUniform1i(glGetUniformLocation(g_output_filter_program, "uSsao"), ssao_on);
        glUniform1f(glGetUniformLocation(g_output_filter_program, "uSsaoRadius"),
                    g_pcSsaoRadius * 0.02f);   /* radius key -> UV offset scale */
        glUniform1f(glGetUniformLocation(g_output_filter_program, "uSsaoIntensity"),
                    g_pcSsaoIntensity);
        glUniform1f(glGetUniformLocation(g_output_filter_program, "uSsaoAspect"),
                    src_h > 0 ? (float)src_w / (float)src_h : 1.0f);
        glUniform1f(glGetUniformLocation(g_output_filter_program, "uSsaoProjA"),
                    g_pc_ssao_proj_a);
        glUniform1f(glGetUniformLocation(g_output_filter_program, "uSsaoProjB"),
                    g_pc_ssao_proj_b);
    }
    glUniform1i(glGetUniformLocation(g_output_filter_program, "uFilterMode"),
                filter_mode);
    glUniform1i(glGetUniformLocation(g_output_filter_program, "uFxaa"),
                (apply_post && g_pcFxaa) ? 1 : 0);
    glUniform1f(glGetUniformLocation(g_output_filter_program, "uSharpen"),
                apply_post ? gfx_opengl_output_sharpen() : 0.0f);
    {
        int gp =
            (g_pcGradePresets && g_pcGradeLevelValid) ? 1 : 0;
        glUniform1f(glGetUniformLocation(g_output_filter_program, "uLevelSat"),
                    gp ? g_pcGradeLevelSat : 1.0f);
        glUniform1f(glGetUniformLocation(g_output_filter_program, "uLevelCon"),
                    gp ? g_pcGradeLevelCon : 1.0f);
        glUniform3f(glGetUniformLocation(g_output_filter_program, "uColorTint"),
                    gp ? g_pcGradeLevelTintR : 1.0f,
                    gp ? g_pcGradeLevelTintG : 1.0f,
                    gp ? g_pcGradeLevelTintB : 1.0f);
    }
    glUniform1i(glGetUniformLocation(g_output_filter_program, "uTonemap"),
                (apply_post && g_pcTonemap &&
                 g_pcGradeLevelValid) ? 1 : 0);
    glUniform1i(
        glGetUniformLocation(g_output_filter_program, "uLinearFinish"),
        (apply_post && g_pcRemasterFX &&
         ((g_pcTonemap != 0 && g_pcGradeLevelValid) ||
          (g_pcGradePresets != 0 && g_pcGradeLevelValid) ||
          g_pcBloom != 0 ||
          (g_pcSsao != 0 && g_scene_depth_valid &&
           g_pc_ssao_proj_b != 0.0f) ||
          gfx_opengl_output_saturation() != 1.0f ||
          gfx_opengl_output_contrast() != 1.0f ||
          gfx_opengl_output_brightness() != 0.0f))
            ? 1 : 0);
    glUniform1i(glGetUniformLocation(g_output_filter_program, "uRgb555"),
                apply_post ? gfx_opengl_diag_output_rgb555_mode() : 0);
    glBindVertexArray(g_output_filter_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void gfx_opengl_apply_output_vi_filter(void) {
    GLint viewport[4];
    GLint saved_program = 0;
    GLint saved_vao = 0;
    GLint saved_active_texture = 0;
    GLint saved_texture0 = 0;
    GLint saved_draw_fbo = 0;
    GLint saved_read_fbo = 0;
    GLint saved_array_buffer = 0;
    GLboolean saved_scissor = GL_FALSE;
    GLboolean saved_depth_test = GL_FALSE;
    GLboolean saved_blend = GL_FALSE;
    GLboolean saved_dither = GL_FALSE;
    GLboolean saved_a2c = GL_FALSE;
    GLboolean saved_depth_mask = GL_TRUE;
    int width;
    int height;
    int filter_w = 0;
    int filter_h = 0;
    int logical_w = 0;
    int logical_h = 0;
    bool use_logical_size = false;
    GLuint filter_source_tex;
    int filter_source_w;
    int filter_source_h;
    bool use_vi_filter;
    bool use_color_adjust;
    bool use_bloom;
    bool use_fxaa;
    bool use_sharpen;
    extern SDL_Window *g_sdlWindow;
    int drawable_w = 0;
    int drawable_h = 0;

    glGetIntegerv(GL_VIEWPORT, viewport);
    if (g_sdlWindow != NULL) {
        SDL_GL_GetDrawableSize(g_sdlWindow, &drawable_w, &drawable_h);
    }
    width = drawable_w > 0 ? drawable_w : viewport[2];
    height = drawable_h > 0 ? drawable_h : viewport[3];
    gfx_opengl_check_output_filter_color_diag();
    use_vi_filter = gfx_opengl_output_vi_filter_target(width, height, &filter_w, &filter_h);
    use_color_adjust = gfx_opengl_output_color_adjust_active();
    use_bloom = (g_pcBloom != 0) && g_pcRemasterFX;
    use_fxaa = (g_pcFxaa != 0) && g_pcRemasterFX;
    bool use_tonemap = (g_pcTonemap != 0) && g_pcRemasterFX;
    bool use_ssao = gfx_opengl_output_ssao_active() && g_scene_depth_valid;
    use_sharpen = (gfx_opengl_output_sharpen() > 0.0f);
    if (!use_vi_filter && !use_color_adjust && !use_bloom && !use_fxaa && !use_sharpen &&
        !use_tonemap && !use_ssao) {
        return;
    }
    if (!use_vi_filter) {
        filter_w = width;
        filter_h = height;
    }

    if (width <= 0 || height <= 0 || filter_w <= 0 || filter_h <= 0) {
        return;
    }

    if (filter_w > width || filter_h > height) {
        static int warned_too_large;

        if (!warned_too_large) {
            fprintf(stderr,
                    "[fast3d] Ignoring GE007_DIAG_OUTPUT_VI_FILTER=%dx%d for "
                    "%dx%d framebuffer\n",
                    filter_w, filter_h, width, height);
            fflush(stderr);
            warned_too_large = 1;
        }
        return;
    }

    use_logical_size = gfx_opengl_diag_output_vi_logical_size(&logical_w, &logical_h);
    if (use_logical_size &&
        (logical_w <= 0 || logical_h <= 0 ||
         logical_w > width || logical_h > height ||
         filter_w > logical_w || filter_h > logical_h)) {
        static int warned_logical_invalid;

        if (!warned_logical_invalid) {
            fprintf(stderr,
                    "[fast3d] Ignoring GE007_DIAG_OUTPUT_VI_LOGICAL_SIZE=%dx%d "
                    "for %dx%d framebuffer and %dx%d filter\n",
                    logical_w, logical_h, width, height, filter_w, filter_h);
            fflush(stderr);
            warned_logical_invalid = 1;
        }
        use_logical_size = false;
    }

    glGetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture0);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read_fbo);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_array_buffer);
    saved_scissor = glIsEnabled(GL_SCISSOR_TEST);
    saved_depth_test = glIsEnabled(GL_DEPTH_TEST);
    saved_blend = glIsEnabled(GL_BLEND);
    saved_dither = glIsEnabled(GL_DITHER);
    saved_a2c = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &saved_depth_mask);

    if (!gfx_opengl_ensure_output_filter_program()) {
        glActiveTexture((GLenum)saved_active_texture);
        return;
    }

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDepthMask(GL_FALSE);

    gfx_opengl_ensure_filter_texture(&g_output_filter_copy_tex,
                                     &g_output_filter_copy_w,
                                     &g_output_filter_copy_h,
                                     width, height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);

    filter_source_tex = g_output_filter_copy_tex;
    filter_source_w = width;
    filter_source_h = height;

    if (use_logical_size) {
        gfx_opengl_ensure_filter_texture(&g_output_filter_logical_tex,
                                         &g_output_filter_logical_w,
                                         &g_output_filter_logical_h,
                                         logical_w, logical_h);
        glBindFramebuffer(GL_FRAMEBUFFER, g_output_filter_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               g_output_filter_logical_tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glViewport(0, 0, logical_w, logical_h);
            gfx_opengl_draw_output_filter_texture(g_output_filter_copy_tex,
                                                  width, height,
                                                  logical_w, logical_h,
                                                  2, 0);
            filter_source_tex = g_output_filter_logical_tex;
            filter_source_w = logical_w;
            filter_source_h = logical_h;
        }
    }

    gfx_opengl_ensure_filter_texture(&g_output_filter_low_tex,
                                     &g_output_filter_low_w,
                                     &g_output_filter_low_h,
                                     filter_w, filter_h);
    glBindFramebuffer(GL_FRAMEBUFFER, g_output_filter_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           g_output_filter_low_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glViewport(0, 0, filter_w, filter_h);
        gfx_opengl_draw_output_filter_texture(filter_source_tex,
                                              filter_source_w, filter_source_h,
                                              filter_w, filter_h,
                                              0, 0);

        if (use_logical_size) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   g_output_filter_logical_tex, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                glViewport(0, 0, logical_w, logical_h);
                gfx_opengl_draw_output_filter_texture(g_output_filter_low_tex,
                                                      filter_w, filter_h,
                                                      logical_w, logical_h,
                                                      0, 0);

                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, width, height);
                gfx_opengl_draw_output_filter_texture(g_output_filter_logical_tex,
                                                      logical_w, logical_h,
                                                      width, height,
                                                      3, 1);
            }
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width, height);
            gfx_opengl_draw_output_filter_texture(g_output_filter_low_tex,
                                                  filter_w, filter_h,
                                                  width, height,
                                                  0, 1);
        }
    }

    if (saved_blend) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    if (saved_depth_test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (saved_scissor) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    if (saved_dither) {
        glEnable(GL_DITHER);
    } else {
        glDisable(GL_DITHER);
    }
    if (saved_a2c) {
        glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    } else {
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    }
    glDepthMask(saved_depth_mask);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)saved_draw_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)saved_array_buffer);
    glBindVertexArray((GLuint)saved_vao);
    glUseProgram((GLuint)saved_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)saved_texture0);
    glActiveTexture((GLenum)saved_active_texture);
}

static bool gfx_opengl_init(void) {
    /* glad is already loaded by platform_sdl.c before this is called */
    if (SDL_GL_GetCurrentContext() == NULL || glGetString(GL_VERSION) == NULL) {
        fprintf(stderr, "[fast3d] OpenGL init has no current usable context\n");
        return false;
    }

    glGenVertexArrays(1, &opengl_vao);
    glBindVertexArray(opengl_vao);

    glGenBuffers(1, &opengl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    if (opengl_vao == 0 || opengl_vbo == 0 || glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "[fast3d] OpenGL init could not create core buffers\n");
        return false;
    }

    glDepthFunc(GL_LESS);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    /* Known starting state; gfx_opengl_update_a2c_state manages it thereafter. */
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);

    /* GL_DEPTH_CLAMP: prevents near/far clipping, clamping depth values
     * instead of discarding fragments. Core in OpenGL 3.2+.
     * When enabled, we skip the z*=0.3 depth scaling hack in the vertex
     * shader and CPU clipper, matching the PD port's preferred approach. */
    {
        GLint major = 0, minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);

        bool can_depth_clamp = (major > 3 || (major == 3 && minor >= 2));

        /* Allow override: GE007_NO_DEPTH_CLAMP=1 forces fallback z*=0.3 */
        if (getenv("GE007_NO_DEPTH_CLAMP")) {
            can_depth_clamp = false;
        }

        if (can_depth_clamp) {
            glEnable(GL_DEPTH_CLAMP);
            GLenum err = glGetError();
            if (err == GL_NO_ERROR) {
                g_depth_clamp_enabled = true;
                printf("[fast3d] GL_DEPTH_CLAMP enabled (GL %d.%d)\n", major, minor);
            } else {
                g_depth_clamp_enabled = false;
                printf("[fast3d] GL_DEPTH_CLAMP failed (err=0x%04X), using z*=0.3 fallback\n", err);
            }
        } else {
            g_depth_clamp_enabled = false;
            printf("[fast3d] GL_DEPTH_CLAMP not available (GL %d.%d), using z*=0.3 fallback\n", major, minor);
        }
    }

    printf("[fast3d] OpenGL %s, GLSL %s\n",
           glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    return true;
}

static void gfx_opengl_on_resize(void) {
    g_scene_w = 0;
    g_scene_h = 0;
    g_scene_has_stencil = false;
    g_scene_msaa_w = 0;
    g_scene_msaa_h = 0;
    g_scene_msaa_samples = 0;
    g_scene_msaa_has_stencil = false;
}

static float g_clear_r = 0, g_clear_g = 0, g_clear_b = 0;

void gfx_opengl_set_clear_color(float r, float g, float b) {
    g_clear_r = r; g_clear_g = g; g_clear_b = b;
}

static int wireframe_checked = 0, wireframe_on = 0;

/* §3.5(c) fix: a persistently-incomplete FBO (or a shader compile/link failure,
 * both driver/GPU-dependent and not going to spontaneously start succeeding)
 * used to retry full resource creation every single frame while draws sampled
 * an undefined depth map. After a few consecutive failures, give up for the
 * rest of the run and log once, instead of hammering the driver every frame.
 *
 * I-3 fix: the latch used to be permanent for the rest of the run even if the
 * *reason* for the failure (a specific requested resolution) went away. Record
 * the resolution the streak failed at; if the caller later asks for a
 * different Video.SunShadowRes, or toggles SunShadow off then back on, that's
 * a new situation worth retrying rather than a re-hit of the same failure. */
#define GFX_SHADOW_RESOURCE_MAX_FAILURES 3
static int  s_shadow_resource_fail_count = 0;
static bool s_shadow_resource_perma_fail = false;
static int  s_shadow_resource_fail_res = 0;
static int  s_shadow_resource_fail_layers = 0;
static uint64_t s_shadow_attempted_frames = 0;
static uint64_t s_shadow_complete_frames = 0;
static uint64_t s_shadow_fallback_frames = 0;
static uint64_t s_shadow_resource_failures = 0;

static bool gfx_opengl_shadow_resource_fail(int res, int layers) {
    s_shadow_resource_fail_count++;
    s_shadow_resource_failures++;
    s_shadow_resource_fail_res = res;
    s_shadow_resource_fail_layers = layers;
    if (!s_shadow_resource_perma_fail &&
        s_shadow_resource_fail_count >= GFX_SHADOW_RESOURCE_MAX_FAILURES) {
        s_shadow_resource_perma_fail = true;
        fprintf(stderr,
                "[shadow] giving up on sun-shadow resource creation after %d "
                "consecutive failures at res=%d; Remastered keeps its projected-"
                "decal fallback for the rest of this run (the plan owns the "
                "resolution; MDKR_WORLD_SHADOW=0 disables the map path entirely)\n",
                s_shadow_resource_fail_count, res);
        fflush(stderr);
    }
    return false;
}

/* W1.E3.T2: lazily create the sun-shadow depth FBO + program at resolution `res`. */
static bool gfx_opengl_ensure_shadow_resources(int res, int layers) {
    if (res < 256) res = 2048;
    if (layers < 1 || layers > GFX_SHADOW_MAX_MAPS) {
        return false;
    }
    if (s_shadow_resource_perma_fail &&
        (res != s_shadow_resource_fail_res ||
         layers != s_shadow_resource_fail_layers)) {
        fprintf(stderr,
                "[shadow] requested sun-shadow resolution changed (%d -> %d) since the "
                "last failure streak; retrying resource creation\n",
                s_shadow_resource_fail_res, res);
        s_shadow_resource_perma_fail = false;
        s_shadow_resource_fail_count = 0;
    }
    if (s_shadow_resource_perma_fail) return false;
    if (getenv("MDKR_TEST_WORLD_SHADOW_RESOURCE_FAIL") != NULL) {
        return gfx_opengl_shadow_resource_fail(res, layers);
    }
    if (g_shadow_program == 0) {
#ifdef MGB64_PORTMASTER_GLES
        static const char *vs =
            "#version 320 es\n"
            "precision mediump float;\n"
            "in vec3 aShadowPos;\n"
            "uniform mat4 uShadowMat;\n"
            "void main() { gl_Position = uShadowMat * vec4(aShadowPos, 1.0); }\n";
        static const char *fs = "#version 320 es\nprecision mediump float;\nvoid main() {}\n";
#elif defined(__APPLE__)
        static const char *vs =
            "#version 150\n"
            "in vec3 aShadowPos;\n"
            "uniform mat4 uShadowMat;\n"
            "void main() { gl_Position = uShadowMat * vec4(aShadowPos, 1.0); }\n";
        static const char *fs = "#version 150\nvoid main() {}\n";
#else
        static const char *vs =
            "#version 330 core\n"
            "in vec3 aShadowPos;\n"
            "uniform mat4 uShadowMat;\n"
            "void main() { gl_Position = uShadowMat * vec4(aShadowPos, 1.0); }\n";
        static const char *fs = "#version 330 core\nvoid main() {}\n";
#endif
        GLuint vsh = gfx_opengl_compile_filter_shader(GL_VERTEX_SHADER, vs);
        GLuint fsh = gfx_opengl_compile_filter_shader(GL_FRAGMENT_SHADER, fs);
        if (vsh == 0 || fsh == 0) {
            if (vsh) glDeleteShader(vsh);
            if (fsh) glDeleteShader(fsh);
            return gfx_opengl_shadow_resource_fail(res, layers);
        }
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vsh);
        glAttachShader(prog, fsh);
        glBindAttribLocation(prog, 0, "aShadowPos");
        glLinkProgram(prog);
        glDeleteShader(vsh);
        glDeleteShader(fsh);
        GLint linked = GL_FALSE;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            glDeleteProgram(prog);
            return gfx_opengl_shadow_resource_fail(res, layers);
        }
        g_shadow_program = prog;
        g_shadow_mat_loc = glGetUniformLocation(prog, "uShadowMat");
    }
    if (g_shadow_vbo == 0) glGenBuffers(1, &g_shadow_vbo);
    if (g_shadow_vao == 0) glGenVertexArrays(1, &g_shadow_vao);
    if (g_shadow_fbo == 0) glGenFramebuffers(1, &g_shadow_fbo);
    if (g_shadow_depth_tex == 0) glGenTextures(1, &g_shadow_depth_tex);
    if (g_shadow_tex_res != res || g_shadow_tex_layers != layers) {
        /* §3.8a fix: this bind used to land on whatever texture unit the
         * frontend last left active (behind its per-unit binding cache),
         * clobbering that unit's GL_TEXTURE_2D binding for the rest of the
         * frame -> one glitched frame on first-enable/res change. Do the
         * setup on the shadow feature's own dedicated unit (5, same one the
         * receiver samples from in set_uniforms) and restore the active
         * unit afterwards, same idiom as set_uniforms' shadow-tex bind. */
        GLint prev_active = 0;
        GLint prev_fbo = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D_ARRAY, g_shadow_depth_tex);
        glTexImage3D(
            GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24,
            res, res, layers, 0,
            GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        glTexParameteri(
            GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(
            GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(
            GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(
            GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* Comparison sampling for the receiver (T4): sampler2DShadow / LEQUAL. */
        glTexParameteri(
            GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE,
            GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(
            GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo);
        glFramebufferTextureLayer(
            GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
            g_shadow_depth_tex, 0, 0);
#ifndef MGB64_PORTMASTER_GLES  /* depth-only FBO needs no explicit glDrawBuffer in GLES */
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
#endif
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        glActiveTexture((GLenum)prev_active);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            g_shadow_tex_res = 0;
            g_shadow_tex_layers = 0;
            return gfx_opengl_shadow_resource_fail(res, layers);
        }
        g_shadow_tex_res = res;
        g_shadow_tex_layers = layers;
    }
    return true;
}

/* GE007_DUMP_SHADOW_MAP=1: read back the depth texture, write shadow_map.pgm once,
 * and assert non-blank (a real silhouette has depth variation). Local diag only —
 * never committed (R2). */
static void gfx_opengl_dump_shadow_pgm(void) {
    static int done = 0;
    if (done || g_shadow_tex_res <= 0) return;
    static int want = -1;
    if (want < 0) want = (getenv("GE007_DUMP_SHADOW_MAP") != NULL);
    if (!want) return;
    done = 1;
    int res = g_shadow_tex_res;
    float *depth = (float *)malloc((size_t)res * res * sizeof(float));
    if (!depth) return;
    GLint previous_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo);
    glFramebufferTextureLayer(
        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        g_shadow_depth_tex, 0, 0);
    glReadPixels(
        0, 0, res, res, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous_fbo);
    float mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < res * res; i++) { if (depth[i] < mn) mn = depth[i]; if (depth[i] > mx) mx = depth[i]; }
    FILE *f = fopen("shadow_map.pgm", "wb");
    if (f) {
        fprintf(f, "P5\n%d %d\n255\n", res, res);
        /* FIXED normalization (raw depth -> gray) so the GL and Metal dumps are
         * directly comparable (T3 GL-vs-Metal shadow-map compare). glGetTexImage
         * rows are already bottom-to-top (GL texture origin). */
        for (int i = 0; i < res * res; i++) {
            int v = (int)(depth[i] * 255.0f + 0.5f);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            unsigned char c = (unsigned char)v;
            fwrite(&c, 1, 1, f);
        }
        fclose(f);
    }
    fprintf(stderr, "[SHADOW_MAP] dumped shadow_map.pgm res=%d depth_min=%.5f depth_max=%.5f %s\n",
            res, mn, mx, (mx - mn > 1e-4f) ? "NON-BLANK-OK" : "BLANK-FAIL");
    fflush(stderr);
    free(depth);
}

/* W1.E3.T2: render the previous frame's captured caster geometry into the shadow
 * depth map (depth-only, front-face cull, slope-scaled bias). Runs at the head of
 * start_frame, before the scene target is bound (ring is reset afterwards in
 * gfx_pc.c, preserving the one-frame capture-and-replay latency). */
static size_t gfx_opengl_draw_shadow_ranges(
    const GfxShadowRange *ranges,
    size_t range_count,
    size_t base_vertex,
    size_t segment_vertices,
    int view_index) {
    size_t triangles = 0;
    bool culling = true;

    for (size_t index = 0; index < range_count; index++) {
        const GfxShadowRange *range = &ranges[index];
        size_t first;
        if (range->alpha_mode != GFX_SHADOW_ALPHA_OPAQUE ||
            (range->view_index != UINT8_MAX &&
             range->view_index != (uint8_t)view_index) ||
            range->vertex_count == 0 ||
            range->first_vertex > segment_vertices ||
            range->vertex_count >
                segment_vertices - range->first_vertex ||
            range->first_vertex > (size_t)INT_MAX ||
            range->vertex_count > (size_t)INT_MAX ||
            base_vertex > (size_t)INT_MAX ||
            range->first_vertex > (size_t)INT_MAX - base_vertex) {
            continue;
        }
        /*
         * No culling for any caster: the ROM corpus measures 94% of level
         * triangles as single-sided (BACKFACE_DRAW on 5,263/90,617), so DKR
         * terrain is an open shell whose "front" face is its only face —
         * front-face culling (the classic acne mitigation for closed meshes)
         * silently dropped every sun-facing slope from the maps. Acne is
         * owned by the world-unit comparison bias instead.
         */
        if (culling) {
            glDisable(GL_CULL_FACE);
            culling = false;
        }
        first = base_vertex + range->first_vertex;
        glDrawArrays(
            GL_TRIANGLES, (GLint)first,
            (GLsizei)range->vertex_count);
        triangles += range->vertex_count / 3;
    }
    if (!culling) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
    }
    return triangles;
}

static void gfx_opengl_render_shadow_map(void) {
    const GfxShadowFrame *frame;
    size_t static_bytes;
    size_t dynamic_bytes;
    size_t total_bytes;
    bool view_complete[GFX_SHADOW_MAX_VIEWS] = {true, true, true, true};

    /* §3.5 fix: default to "not ready" and only flip true after a full,
     * successful, non-empty replay below — every early-return path here
     * (feature off/stale matrix, empty ring, resource-creation failure)
     * now leaves the receiver gated off instead of sampling an undefined
     * or stale depth map. */
    g_pc_shadow_map_ready = 0;
    g_pc_shadow_mat_valid = 0;
    g_pc_shadow_view_ready_mask = 0;
    memset(&g_shadow_plan, 0, sizeof(g_shadow_plan));
    g_shadow_receiver_view = -1;

    /* I-3 fix: an off->on toggle of Video.SunShadow is also worth a fresh
     * start — the user re-enabling the feature (e.g. after changing a
     * driver/GPU state, or just retrying) shouldn't inherit a perma-fail
     * latch from a previous session of the feature being on. */
    {
        static int s_prev_sun_shadow = 0;
        if (g_pcSunShadow && !s_prev_sun_shadow) {
            s_shadow_resource_perma_fail = false;
            s_shadow_resource_fail_count = 0;
        }
        s_prev_sun_shadow = g_pcSunShadow;
    }

    if (!g_pcSunShadow) return;
    s_shadow_attempted_frames++;
    frame = gfx_shadow_frame_previous();
    if (!gfx_shadow_build_plan(
            frame, g_pc_sun_dir_world, &g_shadow_plan)) {
        s_shadow_fallback_frames++;
        return;
    }
    if (!gfx_opengl_ensure_shadow_resources(
            (int)g_shadow_plan.budget.resolution,
            (int)g_shadow_plan.budget.map_count)) {
        memset(&g_shadow_plan, 0, sizeof(g_shadow_plan));
        s_shadow_fallback_frames++;
        return;
    }
    if (frame->static_vertex_count > SIZE_MAX / sizeof(GfxShadowVertex) ||
        frame->vertex_count > SIZE_MAX / sizeof(GfxShadowVertex) ||
        (frame->static_vertex_count > 0 &&
         frame->static_vertices == NULL) ||
        (frame->vertex_count > 0 && frame->vertices == NULL) ||
        (frame->static_range_count > 0 &&
         frame->static_ranges == NULL) ||
        (frame->range_count > 0 && frame->ranges == NULL)) {
        memset(&g_shadow_plan, 0, sizeof(g_shadow_plan));
        s_shadow_fallback_frames++;
        return;
    }
    static_bytes =
        frame->static_vertex_count * sizeof(GfxShadowVertex);
    dynamic_bytes = frame->vertex_count * sizeof(GfxShadowVertex);
    if (static_bytes > SIZE_MAX - dynamic_bytes) {
        memset(&g_shadow_plan, 0, sizeof(g_shadow_plan));
        s_shadow_fallback_frames++;
        return;
    }
    total_bytes = static_bytes + dynamic_bytes;
    if (total_bytes == 0 || total_bytes > (size_t)PTRDIFF_MAX) {
        memset(&g_shadow_plan, 0, sizeof(g_shadow_plan));
        s_shadow_fallback_frames++;
        return;
    }

    /* Save GL state we perturb (start_frame re-establishes FBO/viewport/masks). */
    GLint prev_vp[4];
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    GLint prev_vao = 0, prev_array_buf = 0, prev_prog = 0, prev_fbo = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buf);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLboolean prev_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean prev_poff = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prev_depth_mask = GL_TRUE;
    GLboolean prev_color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLint prev_cull_face = GL_BACK;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prev_depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, prev_color_mask);
    glGetIntegerv(GL_CULL_FACE_MODE, &prev_cull_face);
    /* §3.6 fix: this pass sets GL_LESS + a slope-scaled polygon offset below;
     * neither was previously restored, so GL_LESS leaked into next frame's
     * first draws (the frontend depth-mode cache only re-issues glDepthFunc
     * on a cache-value change, not per frame) and decals reused a stale
     * offset. Save both and restore verbatim after the pass. */
    GLint prev_depth_func = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &prev_depth_func);
    GLfloat prev_po_factor = 0.0f, prev_po_units = 0.0f;
    bool upload_complete = false;
    bool error_queue_clear = false;
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &prev_po_factor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &prev_po_units);

    /* Upload the typed static and dynamic streams once; ranges select draws. */
    for (int attempt = 0; attempt < 32; attempt++) {
        if (glGetError() == GL_NO_ERROR) {
            error_queue_clear = true;
            break;
        }
    }
    if (error_queue_clear) {
        glBindVertexArray(g_shadow_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_shadow_vbo);
        glBufferData(
            GL_ARRAY_BUFFER, (GLsizeiptr)total_bytes,
            NULL, GL_STREAM_DRAW);
        if (static_bytes > 0) {
            glBufferSubData(
                GL_ARRAY_BUFFER, 0, (GLsizeiptr)static_bytes,
                frame->static_vertices);
        }
        if (dynamic_bytes > 0) {
            glBufferSubData(
                GL_ARRAY_BUFFER, (GLintptr)static_bytes,
                (GLsizeiptr)dynamic_bytes, frame->vertices);
        }
        upload_complete = glGetError() == GL_NO_ERROR;
    }

    if (upload_complete) {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(
            0, 3, GL_FLOAT, GL_FALSE,
            sizeof(GfxShadowVertex), (void *)0);

        glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo);
        glViewport(0, 0, g_shadow_tex_res, g_shadow_tex_res);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);        /* front-face cull: acne mitigation */
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.0f, 4.0f); /* slope-scaled depth bias */

        glUseProgram(g_shadow_program);
        for (size_t index = 0;
             index < g_shadow_plan.cascade_count;
             index++) {
            const GfxShadowCascade *cascade =
                &g_shadow_plan.cascades[index];
            size_t drawn = 0;
            glFramebufferTextureLayer(
                GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                g_shadow_depth_tex, 0, cascade->map_index);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
                GL_FRAMEBUFFER_COMPLETE) {
                view_complete[cascade->view_index] = false;
                continue;
            }
            glClear(GL_DEPTH_BUFFER_BIT);
            glUniformMatrix4fv(
                g_shadow_mat_loc, 1, GL_TRUE,
                &cascade->world_to_clip[0][0]);
            drawn += gfx_opengl_draw_shadow_ranges(
                frame->static_ranges,
                frame->static_range_count,
                0,
                frame->static_vertex_count,
                cascade->view_index);
            drawn += gfx_opengl_draw_shadow_ranges(
                frame->ranges,
                frame->range_count,
                frame->static_vertex_count,
                frame->vertex_count,
                cascade->view_index);
            if (drawn == 0) {
                view_complete[cascade->view_index] = false;
            }
        }
    }

    /* Restore. */
    glPolygonOffset(prev_po_factor, prev_po_units);
    if (!prev_poff) glDisable(GL_POLYGON_OFFSET_FILL);
    if (!prev_cull) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace((GLenum)prev_cull_face);
    }
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    if (!prev_depth) glDisable(GL_DEPTH_TEST);
    glDepthFunc((GLenum)prev_depth_func);
    glDepthMask(prev_depth_mask);
    glColorMask(
        prev_color_mask[0], prev_color_mask[1],
        prev_color_mask[2], prev_color_mask[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glUseProgram((GLuint)prev_prog);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_array_buf);
    glBindVertexArray((GLuint)prev_vao);
    if (!upload_complete) {
        (void)gfx_opengl_shadow_resource_fail(
            (int)g_shadow_plan.budget.resolution,
            (int)g_shadow_plan.budget.map_count);
        memset(&g_shadow_plan, 0, sizeof(g_shadow_plan));
        s_shadow_fallback_frames++;
        return;
    }
    for (size_t view = 0; view < g_shadow_plan.view_count; view++) {
        if (view_complete[view]) {
            g_pc_shadow_view_ready_mask |= 1u << view;
        }
    }
    g_pc_shadow_map_ready =
        g_pc_shadow_view_ready_mask ==
        ((1u << g_shadow_plan.view_count) - 1u);
    g_pc_shadow_mat_valid = g_pc_shadow_map_ready;
    if (g_pc_shadow_map_ready) {
        s_shadow_complete_frames++;
        s_shadow_resource_fail_count = 0;
    } else {
        s_shadow_fallback_frames++;
    }
    if (g_shadow_plan.cascade_count > 0) {
        memcpy(
            g_pc_shadow_mat,
            g_shadow_plan.cascades[0].world_to_clip,
            sizeof(g_pc_shadow_mat));
    }
    gfx_opengl_dump_shadow_pgm();
}

static bool gfx_opengl_start_frame(void) {
    /*
     * frame_count feeds the SHADER_NOISE hash, so it is a per-IMAGE seed, not a
     * per-frame counter for bookkeeping. A presentation replay redraws the same
     * tick's display list: if it advanced the seed, every noise-dithered texel
     * would land somewhere else and a zero-delta replay could not be
     * pixel-identical to the single-walk baseline (Phase 3 Wave B slice 1).
     * Advance it only for real walks.
     */
    if (!gfx_dkr_replay_pass_active()) {
        frame_count++;
    }
    /* W1.E3.T2: replay the prior frame's captured casters into the sun-shadow depth
     * map before the scene target is bound (capture-and-replay, §4.5). No-op when
     * the feature is off or nothing was captured. */
    gfx_opengl_render_shadow_map();
    g_scene_target_bound = false;
    g_output_overlay_active = false;
    g_scene_target_multisampled = false;
    g_scene_depth_valid = false;
    g_pc_ssao_proj_b = 0.0f;   /* reset per frame; the largest-far scene proj wins */
    g_pc_ssao_proj_x = 0.0f;
    g_pc_ssao_proj_y = 0.0f;
    g_pc_view_inv_valid = 0;   /* W1.E2.T1: recapture the view-inverse each frame */

    if (gfx_opengl_scene_target_enabled() &&
        gfx_opengl_ensure_scene_target((int)gfx_current_dimensions.width,
                                       (int)gfx_current_dimensions.height)) {
        g_scene_target_bound = true;
        g_scene_target_multisampled = gfx_opengl_effective_msaa_samples() > 0;
        glBindFramebuffer(GL_FRAMEBUFFER,
                          g_scene_target_multisampled ? g_scene_msaa_fbo : g_scene_fbo);
        glViewport(0, 0, g_scene_w, g_scene_h);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    if (!wireframe_checked) {
        wireframe_on = (getenv("GE007_WIREFRAME") != NULL);
        wireframe_checked = 1;
    }
#ifndef MGB64_PORTMASTER_GLES  /* glPolygonMode unavailable in GLES */
    if (wireframe_on) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
#endif

    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(g_clear_r, g_clear_g, g_clear_b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
            (gfx_diag_xlu_coverage_stencil_enabled() ? GL_STENCIL_BUFFER_BIT : 0));
    glEnable(GL_SCISSOR_TEST);
    return true;
}

static void gfx_opengl_resolve_scene_target(void) {
    extern SDL_Window *g_sdlWindow;
    int drawable_w = 0;
    int drawable_h = 0;

    if (!g_scene_target_bound || g_scene_fbo == 0) {
        return;
    }

    if (g_sdlWindow != NULL) {
        SDL_GL_GetDrawableSize(g_sdlWindow, &drawable_w, &drawable_h);
    }
    if (drawable_w <= 0 || drawable_h <= 0) {
        drawable_w = g_scene_w;
        drawable_h = g_scene_h;
    }

    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (g_scene_target_multisampled) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_msaa_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_scene_fbo);
        glBlitFramebuffer(0, 0, g_scene_w, g_scene_h,
                          0, 0, g_scene_w, g_scene_h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, g_scene_w, g_scene_h,
                      0, 0, drawable_w, drawable_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, drawable_w, drawable_h);
    /* The single-sample scene depth texture now holds this frame's depth (the
     * MSAA path leaves depth only in the multisample renderbuffer, which SSAO
     * cannot sample — so it is only valid when not multisampled). */
    g_scene_depth_valid = !g_scene_target_multisampled;
    g_scene_target_bound = false;
}

static bool gfx_opengl_begin_output_overlay(void) {
    extern SDL_Window *g_sdlWindow;
    int drawable_w = (int)gfx_output_dimensions.width;
    int drawable_h = (int)gfx_output_dimensions.height;
    GLboolean saved_scissor;
    GLboolean saved_depth_mask;

    if (g_output_overlay_active) {
        return true;
    }
    if (drawable_w <= 0 || drawable_h <= 0) {
        return false;
    }
    if (g_sdlWindow != NULL) {
        int live_w = 0;
        int live_h = 0;
        SDL_GL_GetDrawableSize(g_sdlWindow, &live_w, &live_h);
        if (live_w > 0 && live_h > 0) {
            drawable_w = live_w;
            drawable_h = live_h;
        }
    }

    /* Finish supersampling/MSAA and all world-space post effects first. */
    gfx_opengl_resolve_scene_target();
    gfx_opengl_apply_output_vi_filter();

    /*
     * HUD geometry now targets the physical drawable. The offscreen world
     * depth buffer is intentionally not carried across this boundary: authored
     * 2D content must not be rejected by stale scene depth.
     */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, drawable_w, drawable_h);
    saved_scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &saved_depth_mask);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthMask(saved_depth_mask);
    if (saved_scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    g_output_overlay_active = true;
    return true;
}

static void gfx_opengl_end_frame(void) {
    extern const char *g_dumpFramesDir;

    if (!g_output_overlay_active) {
        gfx_opengl_resolve_scene_target();
        gfx_opengl_apply_output_vi_filter();
    }
    /*
     * DKR does not submit a graphics task on every VI present. GL_BACK therefore
     * alternates between the last completed frame and undefined/older storage
     * after swaps. Capture once, while this composited frame is still defined;
     * platform_dump_frame reuses it on intervening presents.
     */
    if (g_dumpFramesDir != NULL) {
        gfx_opengl_capture_default_framebuffer();
    }
}

static void gfx_opengl_finish_render(void) {
}

static void gfx_opengl_shutdown(void) {
    const int program_count = shader_program_pool_size;
    const bool have_context =
        SDL_GL_GetCurrentContext() != NULL && glGetString(GL_VERSION) != NULL;

    if (have_context) {
        /* Complete queued work before invalidating resources referenced by it.
         * This runs before SDL deletes the context. */
        glFinish();
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);

        for (int i = 0; i < shader_program_pool_size; i++) {
            if (shader_program_pool[i] != NULL &&
                shader_program_pool[i]->opengl_program_id != 0) {
                glDeleteProgram(shader_program_pool[i]->opengl_program_id);
                shader_program_pool[i]->opengl_program_id = 0;
            }
        }

        if (opengl_vbo != 0) glDeleteBuffers(1, &opengl_vbo);
        if (opengl_vao != 0) glDeleteVertexArrays(1, &opengl_vao);

        if (g_scene_fbo != 0) glDeleteFramebuffers(1, &g_scene_fbo);
        if (g_scene_color_tex != 0) glDeleteTextures(1, &g_scene_color_tex);
        if (g_scene_depth_tex != 0) glDeleteTextures(1, &g_scene_depth_tex);
        if (g_scene_msaa_fbo != 0) glDeleteFramebuffers(1, &g_scene_msaa_fbo);
        if (g_scene_msaa_color_rb != 0) {
            glDeleteRenderbuffers(1, &g_scene_msaa_color_rb);
        }
        if (g_scene_msaa_depth_rb != 0) {
            glDeleteRenderbuffers(1, &g_scene_msaa_depth_rb);
        }
        if (g_diag_framebuffer_snapshot_tex != 0) {
            glDeleteTextures(1, &g_diag_framebuffer_snapshot_tex);
        }

        if (g_output_filter_copy_tex != 0) {
            glDeleteTextures(1, &g_output_filter_copy_tex);
        }
        if (g_output_filter_low_tex != 0) {
            glDeleteTextures(1, &g_output_filter_low_tex);
        }
        if (g_output_filter_logical_tex != 0) {
            glDeleteTextures(1, &g_output_filter_logical_tex);
        }
        if (g_output_filter_fbo != 0) {
            glDeleteFramebuffers(1, &g_output_filter_fbo);
        }
        if (g_output_filter_program != 0) {
            glDeleteProgram(g_output_filter_program);
        }
        if (g_output_filter_vao != 0) {
            glDeleteVertexArrays(1, &g_output_filter_vao);
        }

        if (g_shadow_fbo != 0) glDeleteFramebuffers(1, &g_shadow_fbo);
        if (g_shadow_depth_tex != 0) glDeleteTextures(1, &g_shadow_depth_tex);
        if (g_shadow_program != 0) glDeleteProgram(g_shadow_program);
        if (g_shadow_vbo != 0) glDeleteBuffers(1, &g_shadow_vbo);
        if (g_shadow_vao != 0) glDeleteVertexArrays(1, &g_shadow_vao);
    } else if (program_count != 0 || opengl_vbo != 0 || opengl_vao != 0) {
        fprintf(stderr,
                "[fast3d] WARNING: GL shutdown ran without a current context; "
                "CPU ownership will be released but GPU deletion was unavailable\n");
    }

    for (int i = 0; i < shader_program_pool_size; i++) {
        free(shader_program_pool[i]);
    }
    free(shader_program_pool);
    shader_program_pool = NULL;
    shader_program_pool_size = 0;
    shader_program_pool_cap = 0;
    current_shader_program = NULL;

    opengl_vbo = 0;
    opengl_vao = 0;
    g_scene_fbo = 0;
    g_scene_color_tex = 0;
    g_scene_depth_tex = 0;
    g_scene_msaa_fbo = 0;
    g_scene_msaa_color_rb = 0;
    g_scene_msaa_depth_rb = 0;
    g_scene_w = g_scene_h = 0;
    g_scene_msaa_w = g_scene_msaa_h = 0;
    g_scene_msaa_samples = 0;
    g_scene_has_stencil = false;
    g_scene_msaa_has_stencil = false;
    g_scene_target_bound = false;
    g_scene_target_multisampled = false;
    g_scene_depth_valid = false;
    g_diag_framebuffer_snapshot_tex = 0;

    g_output_filter_copy_tex = 0;
    g_output_filter_low_tex = 0;
    g_output_filter_logical_tex = 0;
    g_output_filter_fbo = 0;
    g_output_filter_program = 0;
    g_output_filter_vao = 0;
    g_output_filter_copy_w = g_output_filter_copy_h = 0;
    g_output_filter_low_w = g_output_filter_low_h = 0;
    g_output_filter_logical_w = g_output_filter_logical_h = 0;

    g_shadow_fbo = 0;
    g_shadow_depth_tex = 0;
    g_shadow_tex_res = 0;
    g_shadow_tex_layers = 0;
    g_shadow_program = 0;
    g_shadow_mat_loc = -1;
    g_shadow_vbo = 0;
    g_shadow_vao = 0;
    memset(&g_shadow_plan, 0, sizeof(g_shadow_plan));
    g_shadow_receiver_view = -1;
    g_pc_shadow_map_ready = 0;
    g_pc_shadow_view_ready_mask = 0;
    g_output_overlay_active = false;
    g_depth_clamp_enabled = false;
    free(g_capture_frame_buf);
    g_capture_frame_buf = NULL;
    g_capture_frame_w = 0;
    g_capture_frame_h = 0;
    g_capture_frame_valid = false;

    fprintf(stderr,
            "[GL-SHUTDOWN] context=%d programs=%d livePrograms=0 "
            "liveCoreBuffers=0 liveTargets=0 cpuCapture=0\n",
            have_context ? 1 : 0, program_count);
    fprintf(stderr,
            "[WORLD-SHADOW] backend=gl attempted=%llu complete=%llu "
            "fallback=%llu resourceFailures=%llu latched=%d\n",
            (unsigned long long)s_shadow_attempted_frames,
            (unsigned long long)s_shadow_complete_frames,
            (unsigned long long)s_shadow_fallback_frames,
            (unsigned long long)s_shadow_resource_failures,
            s_shadow_resource_perma_fail ? 1 : 0);
}

struct GfxRenderingAPI gfx_opengl_api = {
    .z_is_from_0_to_1 = gfx_opengl_z_is_from_0_to_1,
    .unload_shader = gfx_opengl_unload_shader,
    .load_shader = gfx_opengl_load_shader,
    .create_and_load_new_shader = gfx_opengl_create_and_load_new_shader,
    .lookup_shader = gfx_opengl_lookup_shader,
    .shader_get_info = gfx_opengl_shader_get_info,
    .new_texture = gfx_opengl_new_texture,
    .delete_texture = gfx_opengl_delete_texture,
    .select_texture = gfx_opengl_select_texture,
    .upload_texture = gfx_opengl_upload_texture,
    .set_sampler_parameters = gfx_opengl_set_sampler_parameters,
    .set_depth_mode = gfx_opengl_set_depth_mode,
    .set_viewport = gfx_opengl_set_viewport,
    .set_scissor = gfx_opengl_set_scissor,
    .set_shadow_view = gfx_opengl_set_shadow_view,
    .set_blend_mode = gfx_opengl_set_blend_mode,
    .draw_triangles = gfx_opengl_draw_triangles,
    .read_framebuffer_rgb = gfx_opengl_read_framebuffer_rgb,
    .init = gfx_opengl_init,
    .on_resize = gfx_opengl_on_resize,
    .start_frame = gfx_opengl_start_frame,
    .begin_output_overlay = gfx_opengl_begin_output_overlay,
    .end_frame = gfx_opengl_end_frame,
    .finish_render = gfx_opengl_finish_render,
    .draw_modern_mesh = NULL,
    .upload_texture_mipped = gfx_opengl_upload_texture_mipped,
    .shutdown = gfx_opengl_shutdown,
};
