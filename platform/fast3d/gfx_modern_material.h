#ifndef GFX_MODERN_MATERIAL_H
#define GFX_MODERN_MATERIAL_H
/*
 * The material a modern mesh inherits from the sprite billboard it replaces.
 *
 * The billboard's combiner modulates the texture by PRIM (rgb and the fade
 * alpha). With fog its second cycle is G_CC_BLEND_ENV_ALPHA2,
 * (ENV - COMBINED) * ENV_ALPHA + COMBINED: shaded objects lerp toward the
 * level light colour by its intensity. The mesh shader applies the same lerp
 * when, and only when, that second cycle is active.
 */
#include <stdint.h>
#include "gfx_rendering_api.h"

/* The second cycle's RGB selectors of a G_SETCOMBINE command (w0, w1), packed
 * a | b << 4 | c << 8 | d << 13 for (a - b) * c + d. The DKR renderer
 * normalizes every combine through this (dkr_dp_set_combine), so the packed
 * value is also bits 28..43 of its cc_id. */
static inline uint32_t gfx_combine_cycle2_rgb(uint32_t w0, uint32_t w1) {
    const uint32_t a = (w0 >> 5) & 0xFu, b = (w1 >> 24) & 0xFu;
    const uint32_t c = w0 & 0x1Fu, d = (w1 >> 6) & 0x7u;
    return a | (b << 4) | (c << 8) | (d << 13);
}

/* cycle2_rgb is that packed selector set; two_cycle the other-mode cycle
 * type. */
static inline void gfx_modern_material_resolve(uint32_t cycle2_rgb, int two_cycle,
                                               const uint8_t prim[4],
                                               const uint8_t env[4],
                                               struct GfxModernMeshMaterial *out) {
    /* (G_CCMUX_ENVIRONMENT - G_CCMUX_COMBINED) * G_CCMUX_ENV_ALPHA + G_CCMUX_COMBINED */
    const int env_blend = two_cycle && cycle2_rgb == (5u | (0u << 4) | (12u << 8) | (0u << 13));
    for (int i = 0; i < 4; ++i) {
        out->tint[i] = prim[i] / 255.0f;
        out->env[i] = env_blend ? env[i] / 255.0f : 0.0f;
    }
}

#endif
