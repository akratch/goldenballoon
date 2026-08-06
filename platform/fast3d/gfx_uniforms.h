/*
 * gfx_uniforms.h — shared render/post-FX uniform state.
 *
 * These globals are the video/remaster tunables the frontend resolves (from
 * settings + GE007_* overrides) and the renderer backends consume. They are
 * defined once (in gfx_pc.c) and read by every backend. Declaring them here, in
 * one place included by both the OpenGL and Metal backends, keeps the two from
 * independently re-declaring the same symbols with types that can silently
 * drift apart.
 *
 * NOTE: the definitions in gfx_pc.c must match these declarations; that TU
 * includes this header so the compiler enforces it.
 */
#ifndef GFX_UNIFORMS_H
#define GFX_UNIFORMS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Output / color pipeline. */
extern float g_pcVideoGamma;
extern float g_pcRenderScale;
extern int   g_pcMsaaSamples;
extern int   g_pcTextureAnisotropy;   /* Video.AnisotropicFiltering: 1=faithful, >1=hw aniso for grazing surfaces */
extern int   g_pcRetroFilterMode;
extern float g_pcVideoSaturation;
extern float g_pcVideoContrast;
extern float g_pcVideoBrightness;
extern int   g_pcOutputDither;
extern float g_pcVignette;

/* Bloom. */
extern int   g_pcBloom;
extern float g_pcBloomThreshold;
extern float g_pcBloomIntensity;

/* SSAO. */
extern int   g_pcSsao;
extern int   g_pcSsaoMode;            /* 1=planar v1, 2=hemisphere v2 (Metal-only) */
extern float g_pcSsaoRadius;
extern float g_pcSsaoIntensity;
extern float g_pcSsaoBias;            /* horizon elevation bias (self-occlusion reject) */
extern float g_pcSsaoPower;           /* AO contrast exponent */
extern float g_pcSsaoFarCutoff;       /* world-Z beyond which AO fades to 0 */
extern float g_pcSsaoNearCut;         /* depth <= this = viewmodel/near, no AO */
extern float g_pcSsaoSkyCut;          /* depth >= this = sky, no AO */
extern int   g_pcSsaoHalfRes;         /* render AO at half scene res */
extern int   g_pcSsaoBlur;            /* separable bilateral blur pass */
extern float g_pcSsaoBlurDepthSharp;  /* bilateral depth-weight sharpness */
extern float g_pc_ssao_proj_a;        /* scene projection A=P[2][2] (depth linearization) */
extern float g_pc_ssao_proj_b;        /* scene projection B=P[3][2] */
extern float g_pc_ssao_proj_x;        /* scene projection P[0][0] (view-ray x) */
extern float g_pc_ssao_proj_y;        /* scene projection P[1][1] (view-ray y) */

/* Per-pixel directional light / relight. */
extern int   g_pc_view_inv_valid;     /* view-inverse capture latch (reset per frame) */
extern float g_pc_sun_dir_world[3];   /* normalized GlobalLight dir, world space (dir TO light) */
extern float g_pc_sun_color_linear[3];/* level-derived, exposure-normalized linear RGB */
extern float g_pcSunStrength;         /* restrained additive diffuse fraction */
extern int   g_pcLevelLightingValid;  /* level header successfully published */
extern float g_pcEnvRelightBlend;     /* retained experimental RL-1 blend dial */
extern int   g_pcPerPixelLight;       /* Remastered RL-5 smooth-normal directional sun */

/* SMAA (Metal backend post-FX). */
extern int   g_pcSmaa;

/* Sun shadow map (capture-and-replay). */
extern int   g_pcSunShadow;
extern float g_pcSunShadowBias;
extern float g_pcSunShadowUmbra;
extern float g_pc_shadow_mat[4][4];   /* world->shadow-clip, m[row][col] */
extern int   g_pc_shadow_mat_valid;
extern int   g_pc_shadow_map_ready;   /* frontend-visible, set after a non-empty depth-pass replay */
extern unsigned g_pc_shadow_view_ready_mask;
extern const float *gfx_shadow_get_geometry(size_t *out_tri_count);

/* Output-FX chain. */
extern int   g_pcFxaa;
extern float g_pcSharpen;
extern int   g_pcGradePresets;
extern int   g_pcTonemap;
extern int   g_pcRemasterFX;
extern float g_pcGradeLevelSat;
extern float g_pcGradeLevelCon;
extern float g_pcGradeLevelTintR;
extern float g_pcGradeLevelTintG;
extern float g_pcGradeLevelTintB;
extern int   g_pcGradeLevelValid;

#ifdef __cplusplus
}
#endif

#endif /* GFX_UNIFORMS_H */
