/*
 * gfx_mipgen.h — backend-agnostic mip chain construction.
 *
 * WHY THIS EXISTS. Mipmaps were disabled on every backend in this port.
 * gfx_opengl.c:1744-48 dropped them because N64 textures are NPOT (32x48 and
 * similar) and glGenerateMipmap fails *silently* on macOS Metal for those,
 * leaving an incomplete texture; the WebGPU backend then copied the workaround
 * ("single-level like the GL path"). For a kart racer that is the worst place
 * to lose them: the track surface is permanently at a grazing angle receding to
 * the horizon, and the resulting shimmer is the single artifact that most reads
 * as "emulator" rather than "remaster".
 *
 * Building the chain on the CPU sidesteps the driver bug by construction rather
 * than working around it, and it gives all three backends byte-identical levels
 * from one implementation.
 *
 * The module is pure — no GL, no WebGPU, no Metal, no globals — so
 * tests/test_mip_chain.c asserts every rule below without a device.
 *
 * TWO RULES THAT ARE EASY TO GET WRONG:
 *
 *  1. Filter in LINEAR light, not in sRGB bytes. Averaging the encoded bytes of
 *     black and white gives 127; averaging the light they represent gives ~188.
 *     The first is the classic "mipmaps look muddy" bug. Alpha is a coverage
 *     fraction, not a light value, so it averages linearly with no transfer
 *     function applied.
 *
 *  2. Use an exact-AREA box, not a fixed 2-tap. For odd dimensions a 2-tap box
 *     silently drops the last row or column. The area box integrates over
 *     [i*src/dst, (i+1)*src/dst) with fractional edge weights, so every source
 *     texel contributes its true share.
 *
 * A note on wrap modes: an area box never samples outside the source interval,
 * so it needs no clamp/wrap edge rule and does not take one. That would only
 * matter for a kernel wider than the interval (tent, Kaiser).
 */
#ifndef MDKR64_GFX_MIPGEN_H
#define MDKR64_GFX_MIPGEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Video.Mipmaps, published by platform/video_config_runtime.c. Declared here
 * rather than in the vendored gfx_uniforms.h so the mgb64 header stays a clean
 * mirror of upstream; defined in platform/gfx_config_shim.c with the rest of
 * the render globals.
 */
extern int g_pcMipmaps;

/*
 * Set by the F3DDKR interpreter for the duration of one set_sampler_parameters
 * call when the draw is screen-space 2D (TEXRECT/FILLRECT). Backends must then
 * pin the sampler to level 0 regardless of whether the bound texture carries a
 * chain — the HUD and menus must not be mipmapped.
 */
extern int g_gfxSamplerLod0Only;

/* Enough for a 4096x4096 source; N64 textures never approach this. */
#define GFX_MIP_MAX_LEVELS 13

typedef struct GfxMipChain {
    int level_count;
    int width[GFX_MIP_MAX_LEVELS];
    int height[GFX_MIP_MAX_LEVELS];
    /* level[0] ALIASES the caller's source buffer; levels 1.. point into the
     * caller's scratch. Nothing here owns memory. */
    const uint8_t *level[GFX_MIP_MAX_LEVELS];
} GfxMipChain;

/* Levels a width x height texture yields, halving (floored at 1) until 1x1.
 * Returns 0 for degenerate dimensions. */
int gfx_mip_level_count(int width, int height);

/* Scratch bytes needed for levels 1..N-1. Level 0 is the caller's buffer and
 * is never copied, so it is not counted. */
size_t gfx_mip_chain_bytes(int width, int height);

/*
 * Build the chain from `src_rgba` (width*height*4, RGBA8) into `scratch`.
 * Returns false on bad arguments or insufficient scratch — never overruns.
 */
bool gfx_mip_build(const uint8_t *src_rgba, int width, int height,
                   uint8_t *scratch, size_t scratch_bytes,
                   GfxMipChain *out);

/*
 * As gfx_mip_build, but for ALPHA-TESTED CUTOUT materials (foliage, fences,
 * grates — anything drawn with a discard rather than a blend).
 *
 * Box-filtering alpha shrinks a cutout at every level: the mean of an opaque
 * and a transparent texel lands below the alpha threshold, so the shape erodes
 * with distance and eventually disappears. This is why fences go see-through
 * in the middle distance in a lot of ports.
 *
 * The fix (Castano): after reducing a level, rescale its alpha so the fraction
 * of texels at or above `alpha_threshold` matches level 0's fraction. The scale
 * is found by bisection per level.
 *
 * `alpha_threshold` of 0 disables the pass, making this identical to
 * gfx_mip_build.
 */
bool gfx_mip_build_cutout(const uint8_t *src_rgba, int width, int height,
                          uint8_t *scratch, size_t scratch_bytes,
                          uint8_t alpha_threshold,
                          GfxMipChain *out);

/* Fraction of texels in `level` whose alpha is >= `threshold`, in [0,1].
 * Exposed because it is the thing the cutout pass conserves, and a caller
 * (or a test) that cannot measure it cannot verify the pass did anything. */
float gfx_mip_alpha_coverage(const uint8_t *rgba, int width, int height,
                             uint8_t threshold);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_GFX_MIPGEN_H */
