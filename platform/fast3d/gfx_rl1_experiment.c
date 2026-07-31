#include "gfx_rl1_experiment.h"

#include <math.h>
#include <stddef.h>

/*
 * A single neutral diagnostic sun, pointing mostly upward. It is intentionally
 * not read from a level table: deriving a real per-world rig is RL-2, and doing
 * that here would confound the vertex-colour decision this experiment isolates.
 */
static const float k_sun[3] = {
    0.3504559f,
    0.8511071f,
   -0.4005210f,
};

static uint8_t rl1_u8(float value) {
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 255.0f) {
        return 255;
    }
    return (uint8_t) (value + 0.5f);
}

const char *gfx_rl1_arm_name(GfxRl1Arm arm) {
    switch (arm) {
        case GFX_RL1_BAKED_SUN:
            return "baked-sun";
        case GFX_RL1_SUPERSEDE:
            return "supersede";
        case GFX_RL1_BAKED:
        default:
            return "baked";
    }
}

bool gfx_rl1_apply_triangle(
    GfxRl1Arm arm,
    const float xyz[9],
    uint8_t rgba[12]) {
    float ab[3];
    float ac[3];
    float normal[3];
    float length_sq;
    float inverse_length;
    float ndl;

    if (arm == GFX_RL1_BAKED || xyz == NULL || rgba == NULL) {
        return false;
    }
    for (int axis = 0; axis < 3; axis++) {
        ab[axis] = xyz[3 + axis] - xyz[axis];
        ac[axis] = xyz[6 + axis] - xyz[axis];
        if (!isfinite(ab[axis]) || !isfinite(ac[axis])) {
            return false;
        }
    }
    normal[0] = ab[1] * ac[2] - ab[2] * ac[1];
    normal[1] = ab[2] * ac[0] - ab[0] * ac[2];
    normal[2] = ab[0] * ac[1] - ab[1] * ac[0];
    length_sq =
        normal[0] * normal[0] +
        normal[1] * normal[1] +
        normal[2] * normal[2];
    if (!isfinite(length_sq) || length_sq <= 1.0e-12f) {
        return false;
    }
    inverse_length = 1.0f / sqrtf(length_sq);
    for (int axis = 0; axis < 3; axis++) {
        normal[axis] *= inverse_length;
    }

    /*
     * DKR batches are not guaranteed to use one winding convention across
     * terrain and objects. Orient the diagnostic face toward the upper
     * hemisphere so the same physical track plane does not invert merely
     * because its source triangle is wound differently.
     */
    if (normal[1] < 0.0f) {
        normal[0] = -normal[0];
        normal[1] = -normal[1];
        normal[2] = -normal[2];
    }
    ndl =
        normal[0] * k_sun[0] +
        normal[1] * k_sun[1] +
        normal[2] * k_sun[2];
    if (ndl < 0.0f) {
        ndl = 0.0f;
    } else if (ndl > 1.0f) {
        ndl = 1.0f;
    }

    for (int vertex = 0; vertex < 3; vertex++) {
        uint8_t *colour = &rgba[vertex * 4];
        if (arm == GFX_RL1_BAKED_SUN) {
            /*
             * The baked signal remains the low-frequency base. The weights
             * intentionally sum above one at full sun: this arm tests adding
             * directionality on top, including the clipping risk on bright art.
             */
            for (int channel = 0; channel < 3; channel++) {
                colour[channel] = rl1_u8(
                    0.78f * (float) colour[channel] +
                    0.32f * 255.0f * ndl);
            }
        } else {
            uint8_t light = rl1_u8(255.0f * (0.58f + 0.42f * ndl));
            colour[0] = light;
            colour[1] = light;
            colour[2] = light;
        }
    }
    return true;
}
