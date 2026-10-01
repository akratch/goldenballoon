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

/* cycle2_* are the second cycle's RGB combiner selectors (G_CCMUX_*):
 * a - b, times c, plus d. two_cycle is the other-mode cycle type. */
static inline void gfx_modern_material_resolve(unsigned cycle2_a, unsigned cycle2_b,
                                               unsigned cycle2_c, unsigned cycle2_d,
                                               int two_cycle, const uint8_t prim[4],
                                               const uint8_t env[4],
                                               struct GfxModernMeshMaterial *out) {
    /* G_CCMUX_ENVIRONMENT, G_CCMUX_COMBINED, G_CCMUX_ENV_ALPHA */
    const int env_blend = two_cycle && cycle2_a == 5u && cycle2_b == 0u &&
                          cycle2_c == 12u && cycle2_d == 0u;
    for (int i = 0; i < 4; ++i) {
        out->tint[i] = prim[i] / 255.0f;
        out->env[i] = env_blend ? env[i] / 255.0f : 0.0f;
    }
}

#endif
