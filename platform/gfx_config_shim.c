/**
 * gfx_config_shim.c — definitions of the mgb64 render/post-FX config globals
 * that the vendored OpenGL backend (platform/fast3d/gfx_opengl.c) reads.
 *
 * In mgb64 these live in config_pc.c / gfx_pc.c, resolved from the GE settings
 * UI + GE007_* env overrides. mdkr64 has no equivalent settings surface for
 * most of them, so this file defines them once as storage with neutral /
 * post-FX-OFF defaults. The presentation-mode set is overwritten at startup by
 * mdkr_video_config_publish() (platform/video_config_runtime.c) out of the
 * preset table in platform/video_config.c: g_pcRemasterFX and what it gates
 * (g_pcGradePresets, g_pcTonemap, g_pcPerPixelLight and the g_pcSunShadow*
 * group), plus g_pcRenderScale, g_pcMsaaSamples, g_pcTextureAnisotropy and
 * g_pcMipmaps — see the NOTE below. The rest (bloom, SSAO, FXAA, retro filter,
 * vignette, dither, and the gamma/saturation/contrast/brightness controls) stay
 * at their neutral defaults; nothing in this port currently drives them.
 *
 * Types mirror platform/fast3d/gfx_uniforms.h (included for compiler checking).
 */
#include <stddef.h>
#include <stdbool.h>

#include "gfx_uniforms.h"

/* Output / color pipeline — neutral.
 *
 * NOTE (videoconf wave): g_pcRenderScale, g_pcMsaaSamples, g_pcTextureAnisotropy
 * and g_pcMipmaps — and, through g_pcRemasterFX, the grade/tonemap, per-pixel
 * lighting and sun-shadow globals further down — are DEFINED here (the backends
 * link against them) but their VALUES are owned by platform/video_config.c,
 * whose resolved presentation mode is published over these initializers during
 * startup. What is
 * written below is only what the program holds between load and
 * mdkr_video_config_publish(). Change the preset table in video_config.c, not
 * these lines. */
float g_pcVideoGamma      = 1.0f;
float g_pcRenderScale     = 1.0f;
int   g_pcMsaaSamples     = 1;      /* 1 = no MSAA */
int   g_pcTextureAnisotropy = 1;
int   g_pcRetroFilterMode = 1;      /* PC_RETRO_FILTER_OFF */
float g_pcVideoSaturation = 1.0f;
float g_pcVideoContrast   = 1.0f;
float g_pcVideoBrightness = 0.0f;
int   g_pcOutputDither    = 0;
int   g_pcMipmaps         = 0;   /* mdkr-specific; owned by video_config.c */
int   g_gfxSamplerLod0Only = 0;  /* mdkr-specific; set per-draw by gfx_pc_dkr.c */
float g_pcVignette        = 0.0f;

/* Bloom — off. */
int   g_pcBloom           = 0;
float g_pcBloomThreshold  = 1.0f;
float g_pcBloomIntensity  = 0.0f;

/* SSAO — off. */
int   g_pcSsao            = 0;
int   g_pcSsaoMode        = 0;
float g_pcSsaoRadius      = 0.0f;
float g_pcSsaoIntensity   = 0.0f;
float g_pcSsaoBias        = 0.0f;
float g_pcSsaoPower       = 1.0f;
float g_pcSsaoFarCutoff   = 0.0f;
float g_pcSsaoNearCut     = 0.0f;
float g_pcSsaoSkyCut      = 1.0f;
int   g_pcSsaoHalfRes     = 0;
int   g_pcSsaoBlur        = 0;
float g_pcSsaoBlurDepthSharp = 0.0f;
float g_pc_ssao_proj_a    = 0.0f;
float g_pc_ssao_proj_b    = 0.0f;
float g_pc_ssao_proj_x    = 0.0f;
float g_pc_ssao_proj_y    = 0.0f;

/* Per-pixel directional light / relight — off. */
int   g_pc_view_inv_valid = 0;
float g_pc_sun_dir_world[3] = { 0.0f, 1.0f, 0.0f };
float g_pc_sun_color_linear[3] = { 1.0f, 1.0f, 1.0f };
float g_pcSunStrength = 0.0f;
int   g_pcLevelLightingValid = 0;
float g_pcEnvRelightBlend = 0.0f;
int   g_pcPerPixelLight   = 0;

/* SMAA (Metal-only) — off. */
int   g_pcSmaa            = 0;

/* Sun shadow — off. */
int   g_pcSunShadow       = 0;
int   g_pcSunShadowRes    = 0;
float g_pcSunShadowBias   = 0.0f;
float g_pcSunShadowUmbra  = 0.0f;
float g_pc_shadow_mat[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
int   g_pc_shadow_mat_valid = 0;
int   g_pc_shadow_map_ready = 0;
unsigned g_pc_shadow_view_ready_mask = 0;

/* Output-FX chain — off / neutral. */
int   g_pcFxaa            = 0;
float g_pcSharpen         = 0.0f;
int   g_pcGradePresets    = 0;
int   g_pcTonemap         = 0;
int   g_pcRemasterFX      = 0;
float g_pcGradeLevelSat   = 1.0f;
float g_pcGradeLevelCon   = 1.0f;
float g_pcGradeLevelTintR = 1.0f;
float g_pcGradeLevelTintG = 1.0f;
float g_pcGradeLevelTintB = 1.0f;
int   g_pcGradeLevelValid = 0;

/* Depth-clamp toggle (gfx_opengl.c both reads and writes this). */
bool  g_depth_clamp_enabled = false;

/* Sun-shadow geometry source — none on the DKR port. */
const float *gfx_shadow_get_geometry(size_t *out_tri_count) {
    if (out_tri_count != NULL) {
        *out_tri_count = 0;
    }
    return NULL;
}
