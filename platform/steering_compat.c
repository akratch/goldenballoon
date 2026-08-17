#include "steering_compat.h"

#include <math.h>

float mdkr_lateral_traction_step(float lateral_velocity,
                                 float steering_force,
                                 float surface_traction,
                                 float pitch_retention,
                                 int enhanced_half_step) {
    float retention;
    float half_retention;

    if (!enhanced_half_step) {
        /* Preserve the retail expression order in Original mode. */
        return (lateral_velocity + steering_force) * surface_traction *
               pitch_retention;
    }

    retention = surface_traction * pitch_retention;
    if (retention <= 0.0f) {
        return 0.0f;
    }
    half_retention = sqrtf(retention);
    return lateral_velocity * half_retention +
           steering_force * retention / (1.0f + half_retention);
}
