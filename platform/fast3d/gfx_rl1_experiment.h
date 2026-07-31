/**
 * RL-1 vertex-colour decision experiment.
 *
 * This is not a production lighting rig. It gives the renderer three precise,
 * backend-independent hypotheses to capture on bright and dark tracks before
 * RL-2 chooses a level-derived sun:
 *
 *   BAKED        original vertex colour, byte-for-byte;
 *   BAKED_SUN    78% baked low-frequency base + 32% directional light;
 *   SUPERSEDE    replace baked RGB with 58% ambient + 42% directional light.
 *
 * The fixed sun exists only to hold the experiment constant between tracks.
 */
#ifndef MDKR64_GFX_RL1_EXPERIMENT_H
#define MDKR64_GFX_RL1_EXPERIMENT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum GfxRl1Arm {
    GFX_RL1_BAKED = 0,
    GFX_RL1_BAKED_SUN,
    GFX_RL1_SUPERSEDE,
} GfxRl1Arm;

/**
 * Apply one RL-1 hypothesis to a triangle's vertex RGB values.
 *
 * xyz is three tightly-packed object/world-space positions. rgba is three
 * tightly-packed four-byte colours; alpha is never modified. Returns false
 * for BAKED or a degenerate/non-finite triangle and leaves rgba untouched.
 */
bool gfx_rl1_apply_triangle(
    GfxRl1Arm arm,
    const float xyz[9],
    uint8_t rgba[12]);

const char *gfx_rl1_arm_name(GfxRl1Arm arm);

#endif
