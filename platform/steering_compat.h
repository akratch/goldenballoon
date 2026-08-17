#ifndef MDKR_STEERING_COMPAT_H
#define MDKR_STEERING_COMPAT_H

float mdkr_lateral_traction_step(float lateral_velocity,
                                 float steering_force,
                                 float surface_traction,
                                 float pitch_retention,
                                 int enhanced_half_step);

#endif
