#ifndef MDKR64_GFX_TEXTURE_EDGE_H
#define MDKR64_GFX_TEXTURE_EDGE_H

/*
 * Authoritative alpha-test boundary for RDP texture-edge materials.
 *
 * The shaders use a strict `alpha > 0.19` comparison. In RGBA8, 48/255 is
 * below that boundary and 49/255 is above it, so mip coverage must count alpha
 * values from 49 upward. Keeping the shader literal and byte boundary together
 * prevents mip generation from preserving a different silhouette than the GPU
 * ultimately draws.
 */
#define MDKR_GFX_STRINGIFY_INNER(value) #value
#define MDKR_GFX_STRINGIFY(value) MDKR_GFX_STRINGIFY_INNER(value)

#define GFX_TEXTURE_EDGE_ALPHA_THRESHOLD_VALUE 0.19
#define GFX_TEXTURE_EDGE_ALPHA_THRESHOLD \
    ((float) GFX_TEXTURE_EDGE_ALPHA_THRESHOLD_VALUE)
#define GFX_TEXTURE_EDGE_ALPHA_THRESHOLD_U8 49u
#define GFX_TEXTURE_EDGE_ALPHA_THRESHOLD_SHADER \
    MDKR_GFX_STRINGIFY(GFX_TEXTURE_EDGE_ALPHA_THRESHOLD_VALUE)

#endif /* MDKR64_GFX_TEXTURE_EDGE_H */
