/*
 * gfx_mipgen.c — see gfx_mipgen.h for why this is a CPU module.
 */
#include "gfx_mipgen.h"

#include <math.h>
#include <string.h>

/* ---- sRGB transfer ------------------------------------------------------
 * N64 texels are not formally sRGB, but they are authored and displayed
 * through a roughly-sRGB pipeline, and treating them as such is what makes a
 * reduced level read as the same material at distance. The forward table is
 * built once; the inverse is evaluated directly (it is called once per
 * destination channel, not per tap).
 */
static float s_srgb_to_linear[256];
static int s_srgb_ready;

static void srgb_table_init(void) {
    if (s_srgb_ready) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        float c = (float) i / 255.0f;
        s_srgb_to_linear[i] = (c <= 0.04045f)
            ? (c / 12.92f)
            : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    s_srgb_ready = 1;
}

static uint8_t linear_to_srgb_u8(float v) {
    float c;
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 1.0f) {
        return 255;
    }
    c = (v <= 0.0031308f) ? (v * 12.92f)
                          : (1.055f * powf(v, 1.0f / 2.4f) - 0.055f);
    return (uint8_t) (c * 255.0f + 0.5f);
}

/* ---- level geometry ---------------------------------------------------- */

static int next_dim(int d) {
    return (d > 1) ? (d >> 1) : 1;
}

int gfx_mip_level_count(int width, int height) {
    int levels = 1;

    if (width <= 0 || height <= 0) {
        return 0;
    }
    while (width > 1 || height > 1) {
        width = next_dim(width);
        height = next_dim(height);
        levels++;
        if (levels >= GFX_MIP_MAX_LEVELS) {
            break;
        }
    }
    return levels;
}

size_t gfx_mip_chain_bytes(int width, int height) {
    int levels = gfx_mip_level_count(width, height);
    size_t bytes = 0;

    for (int l = 1; l < levels; l++) {
        width = next_dim(width);
        height = next_dim(height);
        bytes += (size_t) width * (size_t) height * 4u;
    }
    return bytes;
}

/* ---- the reduction -------------------------------------------------------
 *
 * Exact-area box. Destination texel i covers source interval
 * [i*src/dst, (i+1)*src/dst), which is always within [0, src) — so no sample
 * ever leaves the texture and no wrap/clamp rule is required. Partial source
 * texels at each end contribute their fractional overlap.
 */
static void reduce_level(const uint8_t *src, int sw, int sh,
                         uint8_t *dst, int dw, int dh) {
    const float x_scale = (float) sw / (float) dw;
    const float y_scale = (float) sh / (float) dh;

    for (int y = 0; y < dh; y++) {
        const float y0 = (float) y * y_scale;
        const float y1 = y0 + y_scale;
        const int iy0 = (int) y0;
        const int iy1 = (int) ceilf(y1);

        for (int x = 0; x < dw; x++) {
            const float x0 = (float) x * x_scale;
            const float x1 = x0 + x_scale;
            const int ix0 = (int) x0;
            const int ix1 = (int) ceilf(x1);

            float acc_r = 0.0f, acc_g = 0.0f, acc_b = 0.0f, acc_a = 0.0f;
            float acc_w = 0.0f;

            for (int sy = iy0; sy < iy1 && sy < sh; sy++) {
                /* Overlap of this source row with the destination interval. */
                const float ry0 = (float) sy > y0 ? (float) sy : y0;
                const float ry1 = (float) (sy + 1) < y1 ? (float) (sy + 1) : y1;
                const float wy = ry1 - ry0;
                if (wy <= 0.0f) {
                    continue;
                }
                for (int sx = ix0; sx < ix1 && sx < sw; sx++) {
                    const float rx0 = (float) sx > x0 ? (float) sx : x0;
                    const float rx1 = (float) (sx + 1) < x1 ? (float) (sx + 1) : x1;
                    const float wx = rx1 - rx0;
                    const uint8_t *p;
                    float w;
                    if (wx <= 0.0f) {
                        continue;
                    }
                    w = wx * wy;
                    p = src + ((size_t) sy * (size_t) sw + (size_t) sx) * 4u;
                    /* Colour in linear light; alpha is coverage, so linear as-is. */
                    acc_r += s_srgb_to_linear[p[0]] * w;
                    acc_g += s_srgb_to_linear[p[1]] * w;
                    acc_b += s_srgb_to_linear[p[2]] * w;
                    acc_a += (float) p[3] * w;
                    acc_w += w;
                }
            }

            {
                uint8_t *o = dst + ((size_t) y * (size_t) dw + (size_t) x) * 4u;
                if (acc_w > 0.0f) {
                    const float inv = 1.0f / acc_w;
                    o[0] = linear_to_srgb_u8(acc_r * inv);
                    o[1] = linear_to_srgb_u8(acc_g * inv);
                    o[2] = linear_to_srgb_u8(acc_b * inv);
                    o[3] = (uint8_t) (acc_a * inv + 0.5f);
                } else {
                    o[0] = o[1] = o[2] = o[3] = 0;
                }
            }
        }
    }
}

/* ---- alpha coverage (cutout materials) ---------------------------------- */

float gfx_mip_alpha_coverage(const uint8_t *rgba, int width, int height,
                             uint8_t threshold) {
    size_t n = (size_t) width * (size_t) height;
    size_t hits = 0;

    if (rgba == NULL || width <= 0 || height <= 0) {
        return 0.0f;
    }
    for (size_t i = 0; i < n; i++) {
        if (rgba[i * 4u + 3u] >= threshold) {
            hits++;
        }
    }
    return (float) hits / (float) n;
}

/*
 * Quantize exactly as the write-back does.
 *
 * This has to be shared, not merely equivalent-looking. An earlier version
 * tested the float (`a >= threshold`) while the write-back rounded
 * (`(uint8_t)(a + 0.5f)`). At alpha 64 and scale 1.992 the search believed
 * coverage was 0 (127.5 >= 128 is false) and the write-back produced 128,
 * giving coverage 1 — so the pass chose a scale on the strength of a coverage
 * it then did not produce, and a fading fence came out as a solid wall.
 */
static uint8_t scale_alpha(uint8_t a, float scale) {
    float v = (float) a * scale;
    if (v > 255.0f) {
        v = 255.0f;
    }
    return (uint8_t) (v + 0.5f);
}

/* Coverage this level would have if every alpha were multiplied by `scale`. */
static float coverage_at_scale(const uint8_t *rgba, int width, int height,
                               uint8_t threshold, float scale) {
    size_t n = (size_t) width * (size_t) height;
    size_t hits = 0;

    for (size_t i = 0; i < n; i++) {
        if (scale_alpha(rgba[i * 4u + 3u], scale) >= threshold) {
            hits++;
        }
    }
    return (float) hits / (float) n;
}

/*
 * Find the alpha scale whose coverage is CLOSEST to `target`, then apply it.
 *
 * Two rules, and both are needed — each alone produces a visible artifact:
 *
 *  - CLOSEST, not "smallest scale reaching target". Below the pattern's Nyquist
 *    limit a level goes uniform and the only achievable coverages are 0 and 1.
 *    Bisecting for `coverage >= target` then jumps to 1.0 and the fence becomes
 *    a SOLID WALL. Closest-wins picks 0 there (error 0.25 against 0.75), so the
 *    fence fades instead — which is also what alpha-to-coverage would do.
 *
 *  - Ties resolve UP. One halving above Nyquist the achievable coverages
 *    straddle the target (0 and 0.5 for a 0.25 fence) with equal error. Taking
 *    the lower one makes the fence VANISH at a distance where it is still
 *    clearly resolvable — worse than being a little too thick. So an equal-error
 *    candidate at higher coverage wins.
 *
 * Coverage is a monotone step function of the scale, so a coarse scan finds the
 * right step and a local refine finds its edge. Textures here are small and
 * this runs once per texture at cache fill.
 */
static void preserve_alpha_coverage(uint8_t *rgba, int width, int height,
                                    uint8_t threshold, float target) {
    const float max_scale = 8.0f;
    const int coarse = 64;
    size_t n = (size_t) width * (size_t) height;
    float best_scale = 1.0f;
    float best_err = 1e9f;

    for (int i = 0; i <= coarse; i++) {
        float scale = max_scale * (float) i / (float) coarse;
        float c = coverage_at_scale(rgba, width, height, threshold, scale);
        float err = c > target ? (c - target) : (target - c);
        /* <= keeps the LARGEST scale among equals: scanning upward, an
         * equal-error candidate at higher coverage replaces the lower one. */
        if (err <= best_err) {
            best_err = err;
            best_scale = scale;
        }
    }

    /* Refine within one coarse step, same closest-wins rule. */
    {
        const float step = max_scale / (float) coarse;
        float lo = best_scale - step;
        float hi = best_scale + step;
        if (lo < 0.0f) {
            lo = 0.0f;
        }
        for (int i = 0; i <= 32; i++) {
            float scale = lo + (hi - lo) * (float) i / 32.0f;
            float c = coverage_at_scale(rgba, width, height, threshold, scale);
            float err = c > target ? (c - target) : (target - c);
            if (err <= best_err) {
                best_err = err;
                best_scale = scale;
            }
        }
    }

    if (best_scale == 1.0f) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        rgba[i * 4u + 3u] = scale_alpha(rgba[i * 4u + 3u], best_scale);
    }
}

bool gfx_mip_build(const uint8_t *src_rgba, int width, int height,
                   uint8_t *scratch, size_t scratch_bytes,
                   GfxMipChain *out) {
    return gfx_mip_build_cutout(src_rgba, width, height, scratch, scratch_bytes,
                                0, out);
}

bool gfx_mip_build_cutout(const uint8_t *src_rgba, int width, int height,
                          uint8_t *scratch, size_t scratch_bytes,
                          uint8_t alpha_threshold,
                          GfxMipChain *out) {
    int levels;
    size_t used = 0;
    const uint8_t *prev;
    int pw, ph;

    if (src_rgba == NULL || out == NULL || width <= 0 || height <= 0) {
        return false;
    }
    levels = gfx_mip_level_count(width, height);
    if (levels <= 0) {
        return false;
    }
    if (levels > 1 && (scratch == NULL || scratch_bytes < gfx_mip_chain_bytes(width, height))) {
        return false;
    }

    srgb_table_init();

    memset(out, 0, sizeof(*out));
    out->level_count = levels;
    out->width[0] = width;
    out->height[0] = height;
    out->level[0] = src_rgba;   /* alias: the HLE's decoded buffer, never copied */

    /*
     * Pass 1: build the whole chain, each level reduced from the UNMODIFIED
     * level above it.
     */
    prev = src_rgba;
    pw = width;
    ph = height;
    for (int l = 1; l < levels; l++) {
        const int cw = next_dim(pw);
        const int ch = next_dim(ph);
        uint8_t *dst = scratch + used;

        reduce_level(prev, pw, ph, dst, cw, ch);

        out->width[l] = cw;
        out->height[l] = ch;
        out->level[l] = dst;

        used += (size_t) cw * (size_t) ch * 4u;
        prev = dst;
        pw = cw;
        ph = ch;
    }

    /*
     * Pass 2: rescale alpha per level, against level 0's coverage.
     *
     * This MUST be a separate pass. Rescaling inside the loop above would feed
     * each level's correction into the reduction that produces the next one, so
     * the errors compound and deep levels diverge — measured as a fence that
     * erodes at one level and turns into a solid wall two levels later. Every
     * level is corrected independently from clean data.
     */
    if (alpha_threshold != 0) {
        const float target =
            gfx_mip_alpha_coverage(src_rgba, width, height, alpha_threshold);
        for (int l = 1; l < levels; l++) {
            preserve_alpha_coverage((uint8_t *) out->level[l],
                                    out->width[l], out->height[l],
                                    alpha_threshold, target);
        }
    }
    return true;
}
