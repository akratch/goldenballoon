#include "gfx_rdp_interpolation.h"

#include "gfx_cc.h"

#include <math.h>

uint32_t gfx_rdp_interpolation_options(bool fog_enabled, bool legacy) {
    uint32_t options;

    if (legacy) {
        return 0;
    }
    options = SHADER_OPT_NOPERSPECTIVE_INPUTS;
    if (fog_enabled) {
        options |= SHADER_OPT_NOPERSPECTIVE_FOG;
    }
    return options;
}

uint8_t gfx_rdp_fog_from_clip(float z, float w, int16_t fog_mul,
                              int16_t fog_offset) {
    float fog;

    /*
     * Loaded vertices historically substituted epsilon for zero w. Preserve
     * that defined boundary while making NaN/Inf fail closed instead of relying
     * on an implementation-defined floating-to-integer conversion.
     */
    if (!isfinite(z) || !isfinite(w)) {
        return 0;
    }
    if (w == 0.0f) {
        w = 1.0e-6f;
    }
    fog = (z / w) * (float)fog_mul + (float)fog_offset;
    if (!isfinite(fog) || fog <= 0.0f) {
        return 0;
    }
    if (fog >= 255.0f) {
        return 255;
    }
    return (uint8_t)fog;
}
