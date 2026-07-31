/*
 * gfx_texgen.h — RSP texture-generation (environment mapping) coordinate math.
 *
 * Split out of gfx_sp_vertex() so the formula has a unit-testable surface; see
 * tests/test_texgen_coords.c. There is exactly ONE texgen implementation in the
 * port (all backends, native and web, consume the float u/v this produces), so
 * this header is the whole class.
 *
 * Derivation (F3DEX2 microcode, Mr-Wiseguy/f3dex2 f3dex2.s):
 *
 *     vmudh vPairST, vOne, $v31[5]   ; ST = 0x4000
 *     vmacf vPairST, $v3,  $v21[0h]  ; ST += 0x4000 * dot
 *     ...
 *     vmudm $v3, vPairST, vVpMisc    ; (ST * scale) >> 16
 *
 * ST = 0x4000 * (1 + dot) spans 0x0000..0x7FFF, and vmudm is a >>16, so
 *
 *     coord = 0x4000 * (1 + dot) * scale / 0x10000 = (1 + dot) * scale / 4
 *
 * The coordinate therefore spans only HALF of texture_scaling_factor. GE authors
 * env-mapped materials with scale == tex_width * 64, while one texture span is
 * tex_width * 32 in the 5.5 fixed-point domain, so the full normal sweep covers
 * exactly one copy of the environment map and a camera-facing normal (dot == 0)
 * samples its centre.
 */
#ifndef GFX_TEXGEN_H
#define GFX_TEXGEN_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef GFX_TEXGEN_PI
#define GFX_TEXGEN_PI 3.14159265358979323846f
#endif

/*
 * Compute the texgen S/T coordinate pair, in the same 5.5 fixed-point units the
 * triangle UV path consumes.
 *
 *   dotx/doty  normal . lookat, already divided by 127
 *   s_scale    rsp.texture_scaling_factor.s   (uint16 range)
 *   t_scale    rsp.texture_scaling_factor.t
 *   linear     G_TEXTURE_GEN_LINEAR is set (never true for GE content)
 *   legacy     GE007_TEXGEN_LEGACY=1 — reproduce the pre-fix mapping for A/B
 *
 * Postcondition (non-legacy): 0 <= *out_u <= s_scale/2 <= 32767, likewise for V,
 * so both always fit the int16 staging in gfx_sp_vertex. This is what closes the
 * texgen limb of FID-0020.
 */
static inline void gfx_texgen_coords(float dotx, float doty,
                                     float s_scale, float t_scale,
                                     bool linear, bool legacy,
                                     int32_t *out_u, int32_t *out_v)
{
    const float num = legacy ? 0.5f : 0.25f;
    int32_t u;
    int32_t v;

    /* The dot is a cosine and belongs in [-1, 1]; the RSP saturates it in its
     * s16 accumulator. GE's authored s8 normals are unit-length in practice
     * (measured max |dot| = 0.99), so this is a no-op on shipping content — it
     * matches the hardware and makes the int16 bound above provable. */
    if (dotx < -1.0f) dotx = -1.0f;
    if (dotx > 1.0f) dotx = 1.0f;
    if (doty < -1.0f) doty = -1.0f;
    if (doty > 1.0f) doty = 1.0f;

    if (linear) {
        /* Unreachable on GE content — the game never SETS G_TEXTURE_GEN_LINEAR,
         * it only appears in gSPClearGeometryMode masks — so this branch is
         * unvalidated against console. Matches libultraship/GLideN64. The
         * hardware does not compute an arccosine at all (it evaluates an odd
         * cubic through (-1,0), (0,0.5), (1,1)), so every HLE acos here is an
         * approximation deviating by up to ~5% of the texture. */
        const float div = legacy ? GFX_TEXGEN_PI : 2.0f * GFX_TEXGEN_PI;
        const float dx = legacy ? dotx : -dotx;
        const float dy = legacy ? doty : -doty;

        u = (int32_t)(acosf(dx) * (s_scale / div));
        v = (int32_t)(acosf(dy) * (t_scale / div));
    } else {
        u = (int32_t)((dotx + 1.0f) * (s_scale * num));
        v = (int32_t)((doty + 1.0f) * (t_scale * num));
    }

    if (legacy) {
        /* Spurious S mirror with no counterpart in the F3DEX2 microcode or in
         * any of the nine reference implementations surveyed. Retained only
         * under the legacy A/B flag. */
        u = (int32_t)s_scale - u;
    }

    *out_u = u;
    *out_v = v;
}

#endif /* GFX_TEXGEN_H */
