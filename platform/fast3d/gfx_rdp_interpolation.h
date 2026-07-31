#ifndef MDKR_GFX_RDP_INTERPOLATION_H
#define MDKR_GFX_RDP_INTERPOLATION_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The RDP evaluates shade and fog gradients affinely in screen space. Modern
 * GPU varyings default to perspective-correct interpolation, so the HLE must
 * explicitly request linear/no-perspective interpolation for those channels.
 *
 * legacy is a diagnostic positive-control path. It is never selected by a
 * presentation preset; the runtime exposes it only through
 * MDKR_RDP_GRADIENTS=legacy for fixed-vs-old capture comparisons.
 */
uint32_t gfx_rdp_interpolation_options(bool fog_enabled, bool legacy);

/*
 * Recreate the RSP fog coordinate from a homogeneous clip-space position.
 * Generated near-plane vertices need this recomputation after clipping:
 * interpolating the endpoint fog bytes does not, in general, equal evaluating
 * z/w at the generated vertex.
 */
uint8_t gfx_rdp_fog_from_clip(float z, float w, int16_t fog_mul,
                              int16_t fog_offset);

#endif
