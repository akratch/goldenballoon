/*
 * gfx_webgpu_shader.c — WGSL combiner-shader emitter (see gfx_webgpu_shader.h).
 *
 * Ports gfx_opengl.c's GLSL emitter (gfx_opengl_create_and_load_new_shader) and
 * gfx_metal.mm's MSL port to WGSL. The combiner mux logic (shader_item_to_str /
 * append_formula) is reproduced verbatim in WGSL syntax; the vertex-attribute
 * walk is the same CCFeatures order all three backends use, returned via
 * WgpuShaderInfo so the pipeline's WGPUVertexBufferLayout matches buf_vbo.
 *
 * Task 3 scope: the faithful CORE combiner — 1-/2-cycle formula, tex0/tex1
 * sampling, shade/prim/env inputs, fog, alpha, texture-edge cutout — which
 * covers the vast majority of scene geometry ("recognizable textured geometry").
 * The option flags that only ADD fragment effects (n64 3-point filter, tile
 * mask, shader-side clamp, sun-shadow PCF, smooth-normal directional sun, the
 * diag_*
 * paths) are implemented alongside the core combiner. Their vertex attributes
 * are walked in the same order as GL/Metal so every backend shares one VBO
 * contract.
 * WEB-027 landed frame-varying noise (group 0 @binding(7) per-frame uniform).
 */
#include "gfx_webgpu_shader.h"

#include "gfx_cc.h"
#include "gfx_texture_edge.h"
#include "gfx_uniforms.h"   /* g_pcTextureAnisotropy: aniso bypasses the in-shader 3-point filter */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WGSL type for an N-float attribute/varying. */
static const char *type_str(int size) {
    switch (size) {
        case 1:  return "f32";
        case 2:  return "vec2<f32>";
        case 3:  return "vec3<f32>";
        default: return "vec4<f32>";
    }
}

/* Combiner item -> WGSL expression. Mirrors gfx_opengl.c shader_item_to_str;
 * varyings are struct fields, so inputs read `in.vInputN`. `inputs_have_alpha`
 * is opt_alpha (whether the vInput* are vec4 vs vec3). */
static const char *wgsl_item(uint32_t item, bool with_alpha, bool only_alpha,
                             bool inputs_have_alpha, bool hint_single) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:       return with_alpha ? "vec4<f32>(0.0)" : "vec3<f32>(0.0)";
            case SHADER_INPUT_1: return (with_alpha || !inputs_have_alpha) ? "in.vInput1" : "in.vInput1.rgb";
            case SHADER_INPUT_2: return (with_alpha || !inputs_have_alpha) ? "in.vInput2" : "in.vInput2.rgb";
            case SHADER_INPUT_3: return (with_alpha || !inputs_have_alpha) ? "in.vInput3" : "in.vInput3.rgb";
            case SHADER_INPUT_4: return (with_alpha || !inputs_have_alpha) ? "in.vInput4" : "in.vInput4.rgb";
            case SHADER_INPUT_5: return (with_alpha || !inputs_have_alpha) ? "in.vInput5" : "in.vInput5.rgb";
            case SHADER_INPUT_6: return (with_alpha || !inputs_have_alpha) ? "in.vInput6" : "in.vInput6.rgb";
            case SHADER_INPUT_7: return (with_alpha || !inputs_have_alpha) ? "in.vInput7" : "in.vInput7.rgb";
            case SHADER_TEXEL0:  return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A: return hint_single ? "texVal0.a"
                                     : (with_alpha ? "vec4<f32>(texVal0.a)" : "vec3<f32>(texVal0.a)");
            case SHADER_TEXEL1:  return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_TEXEL1A: return hint_single ? "texVal1.a"
                                     : (with_alpha ? "vec4<f32>(texVal1.a)" : "vec3<f32>(texVal1.a)");
            case SHADER_1:       return with_alpha ? "vec4<f32>(1.0)" : "vec3<f32>(1.0)";
            case SHADER_COMBINED:return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:   return with_alpha ? "vec4<f32>(wgpu_noise(in.clip_pos.xy))"
                                                   : "vec3<f32>(wgpu_noise(in.clip_pos.xy))";
        }
    } else {
        switch (item) {
            case SHADER_0:       return "0.0";
            case SHADER_INPUT_1: return "in.vInput1.a";
            case SHADER_INPUT_2: return "in.vInput2.a";
            case SHADER_INPUT_3: return "in.vInput3.a";
            case SHADER_INPUT_4: return "in.vInput4.a";
            case SHADER_INPUT_5: return "in.vInput5.a";
            case SHADER_INPUT_6: return "in.vInput6.a";
            case SHADER_INPUT_7: return "in.vInput7.a";
            case SHADER_TEXEL0:  return "texVal0.a";
            case SHADER_TEXEL0A: return "texVal0.a";
            case SHADER_TEXEL1:  return "texVal1.a";
            case SHADER_TEXEL1A: return "texVal1.a";
            case SHADER_1:       return "1.0";
            case SHADER_COMBINED:return "texel.a";
            case SHADER_NOISE:   return "wgpu_noise(in.clip_pos.xy)";
        }
    }
    return "0.0";
}

/* append_formula port: single / multiply / mix(lerp3) / lerp4, identical
 * structure to gfx_opengl.c append_formula. */
/* Bounded snprintf-append: writes at *n into a `cap`-sized buffer and advances
 * *n, clamping so an overflow can never underflow `cap - *n` (a huge size_t) or
 * write past the buffer on a subsequent call. Combiners are bounded well under
 * the buffer sizes, so this is defensive insurance, not a live path. */
static void bappend(char *buf, size_t cap, size_t *n, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void bappend(char *buf, size_t cap, size_t *n, const char *fmt, ...) {
    if (*n >= cap) { return; }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (w > 0) { *n += (size_t)w; }
    if (*n >= cap) { *n = cap - 1; }
}

static void wgsl_formula(char *buf, size_t cap, size_t *n, uint8_t c[2][4],
                         bool do_single, bool do_multiply, bool do_mix,
                         bool with_alpha, bool only_alpha, bool opt_alpha) {
    int oa = only_alpha ? 1 : 0;
    if (do_single) {
        bappend(buf, cap, n, "%s",
                wgsl_item(c[oa][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        bappend(buf, cap, n, "%s * %s",
                wgsl_item(c[oa][0], with_alpha, only_alpha, opt_alpha, false),
                wgsl_item(c[oa][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        bappend(buf, cap, n, "mix(%s, %s, %s)",
                wgsl_item(c[oa][1], with_alpha, only_alpha, opt_alpha, false),
                wgsl_item(c[oa][0], with_alpha, only_alpha, opt_alpha, false),
                wgsl_item(c[oa][2], with_alpha, only_alpha, opt_alpha, true));
    } else {
        bappend(buf, cap, n, "(%s - %s) * %s + %s",
                wgsl_item(c[oa][0], with_alpha, only_alpha, opt_alpha, false),
                wgsl_item(c[oa][1], with_alpha, only_alpha, opt_alpha, false),
                wgsl_item(c[oa][2], with_alpha, only_alpha, opt_alpha, true),
                wgsl_item(c[oa][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

#define MODULE_CAP (48 * 1024)

char *gfx_webgpu_build_wgsl(uint64_t shader_id0, uint32_t shader_id1,
                            struct WgpuShaderInfo *info) {
    struct CCFeatures cc;
    gfx_cc_get_features(shader_id0, shader_id1, &cc);

    memset(info, 0, sizeof(*info));
    info->num_inputs = cc.num_inputs;
    info->used_textures[0] = cc.used_textures[0];
    info->used_textures[1] = cc.used_textures[1];
    info->opt_alpha = cc.opt_alpha;
    info->opt_texture_edge = cc.opt_texture_edge;
    info->opt_dfdx_light = cc.opt_dfdx_light;
    info->opt_sun_shadow = cc.opt_sun_shadow;
    /* RDP memory-blend emulation (glass / chain-link fence class). Gated on
     * opt_alpha exactly like gfx_opengl.c (prg->diag_rdp_*_blend at :1609). */
    info->diag_rdp_memory_blend = cc.opt_alpha && cc.diag_rdp_memory_blend;
    info->diag_rdp_cvg_memory_blend = cc.opt_alpha && cc.diag_rdp_cvg_memory_blend;
    bool rdp_mem = info->diag_rdp_memory_blend || info->diag_rdp_cvg_memory_blend;

    const char *interp_in  = cc.noperspective_inputs     ? " @interpolate(linear)" : "";
    const char *interp_uv  = cc.noperspective_texcoords  ? " @interpolate(linear)" : "";
    const char *interp_fog = cc.noperspective_fog        ? " @interpolate(linear)" : "";

    /* Section buffers: VsIn fields, VsOut varying fields, vs_main passthrough. */
    char vsin[6144];  size_t in_n = 0;
    char vsout[6144]; size_t out_n = 0;
    char vsbody[6144];size_t body_n = 0;
    vsin[0] = vsout[0] = vsbody[0] = '\0';

    int loc_in = 0;   /* VsIn @location counter */
    int loc_out = 0;  /* VsOut varying @location counter */
    int off = 0;      /* running float offset within the vertex */
    int na = 0;       /* attribute count */

/* Record a vertex attribute (for the WGPUVertexBufferLayout) + emit its VsIn
 * field. `sz` floats at the current running offset. */
#define ATTR_IN(name, sz)                                                        \
    do {                                                                         \
        if (na >= MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY) {                         \
            fprintf(stderr,                                                      \
                    "[webgpu] combiner attribute metadata exceeds storage "      \
                    "capacity (%d)\n", MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY);      \
            info->num_attrs = na + 1;                                             \
            return NULL;                                                          \
        }                                                                         \
        info->attrs[na].location = loc_in;                                       \
        info->attrs[na].size = (sz);                                             \
        info->attrs[na].offset = off;                                            \
        na++;                                                                    \
        bappend(vsin, sizeof(vsin), &in_n, "  @location(%d) %s : %s,\n",         \
                loc_in, (name), type_str(sz));                                   \
        loc_in++;                                                                \
        off += (sz);                                                            \
    } while (0)

/* Emit a VsOut varying + its vs_main passthrough for an attribute named
 * a<Field> -> v<Field>. */
#define VARY(field, sz, interp)                                                  \
    do {                                                                         \
        bappend(vsout, sizeof(vsout), &out_n, "  @location(%d)%s v%s : %s,\n",   \
                loc_out, (interp), (field), type_str(sz));                       \
        bappend(vsbody, sizeof(vsbody), &body_n, "  out.v%s = in.a%s;\n",        \
                (field), (field));                                               \
        loc_out++;                                                               \
    } while (0)

    /* 1. Clip position (attribute only; becomes @builtin(position), not a varying). */
    ATTR_IN("aVtxPos", 4);

    /* 2. Diag coverage-wrap triangle attrs (rare; consumed for stride). */
    if (cc.opt_alpha && cc.diag_rdp_cvg_memory_blend) {
        ATTR_IN("aDiagTri01", 4); VARY("DiagTri01", 4, " @interpolate(linear)");
        ATTR_IN("aDiagTri2", 2);  VARY("DiagTri2", 2, " @interpolate(linear)");
    }
    /* 3. World position (per-pixel lighting / shadow receiver). */
    if (cc.opt_world_pos) {
        ATTR_IN("aWorldPos", 3); VARY("WorldPos", 3, "");
    }
    if (cc.opt_sun_shadow) {
        bappend(vsout, sizeof(vsout), &out_n,
                "  @location(%d) vShadowDepth : f32,\n", loc_out++);
        bappend(vsbody, sizeof(vsbody), &body_n,
                "  out.vShadowDepth = in.aVtxPos.w;\n");
    }
    /* 4. RL-5 smooth normal + queued object-local light direction. */
    if (cc.opt_dfdx_light) {
        ATTR_IN("aSmoothNormal", 3); VARY("SmoothNormal", 3, "");
        ATTR_IN("aLightDir", 3); VARY("LightDir", 3, "");
    }
    /* 5. Per-texture coords + optional clamp/mask limits. */
    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) continue;
        char nm[24];
        snprintf(nm, sizeof(nm), "aTexCoord%d", i);
        ATTR_IN(nm, 2);
        char fld[24]; snprintf(fld, sizeof(fld), "TexCoord%d", i);
        VARY(fld, 2, interp_uv);
        for (int axis = 0; axis < 2; axis++) {
            char ac = axis == 0 ? 'S' : 'T';
            if (cc.clamp[i][axis]) {
                snprintf(nm, sizeof(nm), "aTexClamp%c%d", ac, i);
                ATTR_IN(nm, 1);
                snprintf(fld, sizeof(fld), "TexClamp%c%d", ac, i);
                VARY(fld, 1, interp_uv);
            }
            if (cc.tile_mask[i][axis]) {
                snprintf(nm, sizeof(nm), "aTexMask%c%d", ac, i);
                ATTR_IN(nm, 1);
                snprintf(fld, sizeof(fld), "TexMask%c%d", ac, i);
                VARY(fld, 1, interp_uv);
            }
        }
    }
    /* 6. Fog (rgb + factor). */
    if (cc.opt_fog) {
        ATTR_IN("aFog", 4); VARY("Fog", 4, interp_fog);
    }
    /* 7. Combiner inputs (shade/prim/env). vec4 with alpha, else vec3. */
    int input_sz = cc.opt_alpha ? 4 : 3;
    for (int i = 0; i < cc.num_inputs; i++) {
        char nm[24]; snprintf(nm, sizeof(nm), "aInput%d", i + 1);
        ATTR_IN(nm, input_sz);
        char fld[24]; snprintf(fld, sizeof(fld), "Input%d", i + 1);
        VARY(fld, input_sz, interp_in);
    }

    info->num_attrs = na;
    info->num_floats = off;

    /* ---- Assemble the module ------------------------------------------- */
    char *out = (char *)malloc(MODULE_CAP);
    if (out == NULL) {
        return NULL;
    }
    size_t n = 0;
#define P(...) bappend(out, MODULE_CAP, &n, __VA_ARGS__)

    /* Structs */
    P("struct VsIn {\n%s};\n", vsin);
    P("struct VsOut {\n  @builtin(position) clip_pos : vec4<f32>,\n%s};\n", vsout);

    /* Vertex shader. Depth: WebGPU clip space is 0..1 (z_is_from_0_to_1 == true),
     * matching the Metal path — the CPU T&L already produces that convention, so
     * the position passes through unscaled (no GL 0.3 z-squash). */
    P("@vertex fn vs_main(in : VsIn) -> VsOut {\n");
    P("  var out : VsOut;\n");
    P("%s", vsbody);
    P("  out.clip_pos = in.aVtxPos;\n");
    P("  return out;\n}\n");

    /* Texture/sampler bindings (group 0). */
    if (cc.used_textures[0]) {
        P("@group(0) @binding(0) var uTex0 : texture_2d<f32>;\n");
        P("@group(0) @binding(1) var uSamp0 : sampler;\n");
    }
    if (cc.used_textures[1]) {
        P("@group(0) @binding(2) var uTex1 : texture_2d<f32>;\n");
        P("@group(0) @binding(3) var uSamp1 : sampler;\n");
    }
    /* RDP memory-blend bindings: scene snapshot ("memory color") at 4/5, and —
     * for the coverage-wrap variant — the GL-convention (bottom-up) viewport
     * uniform at 6 (GLSL uDiagViewport; gfx_opengl.c :1078). The snapshot covers
     * the WHOLE scene target (not the GL viewport-rect copy), so memoryUv is
     * simply fragcoord / textureDimensions with no origin uniform: WebGPU's
     * top-down fragcoord and top-down texture v cancel exactly as GL's
     * bottom-up/bottom-up pair does. */
    if (rdp_mem) {
        P("@group(0) @binding(4) var uSnap : texture_2d<f32>;\n");
        P("@group(0) @binding(5) var uSnapSamp : sampler;\n");
    }
    if (info->diag_rdp_cvg_memory_blend) {
        P("struct DiagU { vp : vec4<f32> };\n");
        P("@group(0) @binding(6) var<uniform> uDiag : DiagU;\n");
    }

    /* WEB-027: frame-varying noise (N64 static / fizz). Only emitted when a
     * combiner slot reads SHADER_NOISE. A term-for-term port of gfx_opengl.c's
     * random()/frame_count path (:806/1151): quantize the fragment coord to
     * N64-row cells (floor(fragcoord * 240/window_height)), fold the per-frame
     * counter in as the hash's third component, then the same sin-dot-fract hash.
     * The frame counter + render height ride in a SINGLE small uniform at
     * group(0) @binding(7), added to the bind-group layout ONLY for noise-using
     * combiners (info->uses_noise) and written once per frame — so noise-free
     * pipelines keep their exact bindings and there is no per-draw uniform. Exact
     * bits may differ from GL only via the backend's sin() implementation — the
     * hash constants (12.9898/78.233/37.719, 143758.5453) and cell quantize are
     * identical to gfx_opengl.c's random() — but the static->animated
     * behaviour matches, which is the requirement. `in.clip_pos.xy` is the WGSL
     * @builtin(position) (top-left origin) — GL's gl_FragCoord is bottom-left, so
     * the cell grid is vertically mirrored; imperceptible for per-frame static. */
    /* AC-3: seed from opt_noise (G_AC_DITHER alpha dissolve) as well as a
     * SHADER_NOISE combiner input — byte-match gfx_opengl.c:1141 — so the noise
     * uniform + wgpu_noise() exist for the alpha-dissolve emitted after the
     * combiner clamp below. */
    bool needs_noise = cc.opt_noise;
    for (int ci = 0; ci < 2 && !needs_noise; ci++)
        for (int cj = 0; cj < 2 && !needs_noise; cj++)
            for (int ck = 0; ck < 4 && !needs_noise; ck++)
                if (cc.c[ci][cj][ck] == SHADER_NOISE) needs_noise = true;
    info->uses_noise = needs_noise;
    if (needs_noise) {
        P("struct NoiseU { params : vec4<f32> };\n");   /* x = frame_count, y = window_height */
        P("@group(0) @binding(7) var<uniform> uNoise : NoiseU;\n");
        P("fn wgpu_noise(p : vec2<f32>) -> f32 {\n"
          "  let cell = floor(p * (240.0 / max(uNoise.params.y, 1.0)));\n"
          "  let value = vec3<f32>(cell, uNoise.params.x);\n"
          "  let r = dot(sin(value), vec3<f32>(12.9898, 78.233, 37.719));\n"
          "  return fract(sin(r) * 143758.5453);\n}\n");
    }
    if (cc.opt_dfdx_light) {
        P("struct LightU { sunColourStrength : vec4<f32> };\n");
        P("@group(0) @binding(8) var<uniform> uLight : LightU;\n");
        P("fn mdkrSrgbToLinear(c : vec3<f32>) -> vec3<f32> {\n"
          "  return mix(c / 12.92, pow((c + vec3<f32>(0.055)) / 1.055, vec3<f32>(2.4)), step(vec3<f32>(0.04045), c));\n"
          "}\n");
        P("fn mdkrLinearToSrgb(value : vec3<f32>) -> vec3<f32> {\n"
          "  let c = max(value, vec3<f32>(0.0));\n"
          "  return mix(c * 12.92, 1.055 * pow(c, vec3<f32>(1.0 / 2.4)) - vec3<f32>(0.055), step(vec3<f32>(0.0031308), c));\n"
          "}\n");
    }
    if (cc.opt_sun_shadow) {
        P("struct ShadowU {\n"
          "  worldToClip : array<mat4x4<f32>, 2>,\n"
          "  params : vec4<f32>,\n"
          "  splitsLayers : vec4<f32>,\n"
          "};\n");
        P("@group(0) @binding(9) var uShadowMap : texture_depth_2d_array;\n");
        P("@group(0) @binding(10) var uShadowSampler : sampler_comparison;\n");
        P("@group(0) @binding(11) var<uniform> uShadow : ShadowU;\n");
        P("fn mdkrShadowCascade(cascade : i32, layer : i32, worldPos : vec3<f32>) -> f32 {\n"
          "  let sc = uShadow.worldToClip[cascade] * vec4<f32>(worldPos, 1.0);\n"
          "  if (sc.w <= 0.0) { return 1.0; }\n"
          "  let ndc = sc.xyz / sc.w;\n"
          "  let suv = vec3<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, ndc.z * 0.5 + 0.5);\n"
          "  if (any(suv < vec3<f32>(0.0)) || any(suv > vec3<f32>(1.0))) { return 1.0; }\n"
          "  var shadow = 0.0;\n"
          "  for (var dy = -1; dy <= 1; dy = dy + 1) {\n"
          "    for (var dx = -1; dx <= 1; dx = dx + 1) {\n"
          "      let offset = vec2<f32>(f32(dx), f32(dy)) * uShadow.params.x;\n"
          "      shadow = shadow + textureSampleCompareLevel(uShadowMap, uShadowSampler, suv.xy + offset, layer, suv.z - uShadow.params.y);\n"
          "    }\n"
          "  }\n"
          /* Fade to lit over the outermost band of the footprint so the far
           * cascade ends softly instead of on a hard sliding line (mirrors
           * the GL receiver exactly). */
          "  let edge = max(abs(suv.x - 0.5), abs(suv.y - 0.5)) * 2.0;\n"
          "  return mix(shadow / 9.0, 1.0, smoothstep(0.92, 1.0, edge));\n"
          "}\n");
    }

    /* Shader-side tile mask (N64 mask-wrap): emit the helpers when any texture
     * axis uses it OR the N64 3-point filter needs them (its taps are mask-wrapped
     * — mask 0 is identity). Ports gfx_opengl.c n64TileMaskAxis/n64TileMaskUv;
     * GLSL mod() = floor-based remainder, so n64mod() must too (WGSL % truncates). */
    /* Aniso (Video.AnisotropicFiltering>1) bypasses the in-shader 3-point filter:
     * those materials sample via hardware textureSample so GPU anisotropy engages. */
    bool needs_filter = (cc.n64_filter[0] || cc.n64_filter[1]) && g_pcTextureAnisotropy <= 1;
    bool uses_mask = cc.tile_mask[0][0] || cc.tile_mask[0][1] ||
                     cc.tile_mask[1][0] || cc.tile_mask[1][1];
    if (uses_mask || needs_filter) {
        P("fn n64mod(x : f32, y : f32) -> f32 { return x - y * floor(x / y); }\n");
        P("fn n64TileMaskAxis(texelCoord : f32, maskPeriod : f32) -> f32 {\n"
          "  let extent = abs(maskPeriod);\n"
          "  if (extent <= 0.5) { return texelCoord; }\n"
          "  var coord = n64mod(texelCoord, select(extent, extent * 2.0, maskPeriod < 0.0));\n"
          "  if (maskPeriod < 0.0 && coord >= extent) { coord = extent * 2.0 - coord; }\n"
          "  return coord;\n}\n");
        P("fn n64TileMaskUv(uv : vec2<f32>, texSize : vec2<f32>, maskS : f32, maskT : f32) -> vec2<f32> {\n"
          "  var tc = uv * texSize;\n"
          "  tc.x = n64TileMaskAxis(tc.x, maskS);\n"
          "  tc.y = n64TileMaskAxis(tc.y, maskT);\n"
          "  return tc / texSize;\n}\n");
    }

    /* N64 3-point ("always 3-point" variant of gfx_opengl.c n64TextureFilter):
     * the RDP's characteristic triangular filter instead of GL bilinear. Uses
     * explicit-LOD sampling so it is valid in any control flow; taps are
     * mask-wrapped (mask 0 = identity). */
    if (needs_filter) {
        P("fn n64Filter3(t : texture_2d<f32>, s : sampler, uv : vec2<f32>, texSize : vec2<f32>, maskS : f32, maskT : f32) -> vec4<f32> {\n"
          "  var offset = fract(uv * texSize - vec2<f32>(0.5));\n"
          "  offset = offset - step(1.0, offset.x + offset.y);\n"
          "  let baseUv = uv - offset / texSize;\n"
          "  let c0 = textureSampleLevel(t, s, n64TileMaskUv(baseUv, texSize, maskS, maskT), 0.0);\n"
          "  let c1 = textureSampleLevel(t, s, n64TileMaskUv(baseUv + vec2<f32>(sign(offset.x), 0.0) / texSize, texSize, maskS, maskT), 0.0);\n"
          "  let c2 = textureSampleLevel(t, s, n64TileMaskUv(baseUv + vec2<f32>(0.0, sign(offset.y)) / texSize, texSize, maskS, maskT), 0.0);\n"
          "  return c0 + abs(offset.x) * (c1 - c0) + abs(offset.y) * (c2 - c0);\n}\n");
    }

    /* RDP coverage-wrap helpers (port of gfx_opengl.c diagEdge / diagInsideTri /
     * diagCoverageSample, :1157-1170). fragGL is the GL-convention (bottom-up)
     * fragment coordinate; the sub-pixel offsets are applied in that space so the
     * 8-sample N64 coverage pattern matches the GLSL arm exactly. */
    if (info->diag_rdp_cvg_memory_blend) {
        P("fn diagEdge(a : vec2<f32>, b : vec2<f32>, p : vec2<f32>) -> f32 {\n"
          "  return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);\n}\n");
        P("fn diagInsideTri(p : vec2<f32>, a : vec2<f32>, b : vec2<f32>, c : vec2<f32>) -> f32 {\n"
          "  let e0 = diagEdge(a, b, p);\n"
          "  let e1 = diagEdge(b, c, p);\n"
          "  let e2 = diagEdge(c, a, p);\n"
          "  let hasNeg = (e0 < 0.0) || (e1 < 0.0) || (e2 < 0.0);\n"
          "  let hasPos = (e0 > 0.0) || (e1 > 0.0) || (e2 > 0.0);\n"
          "  return select(1.0, 0.0, hasNeg && hasPos);\n}\n");
        P("fn diagCoverageSample(fragGL : vec2<f32>, pixelOffset : vec2<f32>, a : vec2<f32>, b : vec2<f32>, c : vec2<f32>) -> f32 {\n"
          "  let p = ((fragGL + pixelOffset - uDiag.vp.xy) / uDiag.vp.zw) * 2.0 - vec2<f32>(1.0);\n"
          "  return diagInsideTri(p, a, b, c);\n}\n");
    }

    /* Fragment shader */
    P("@fragment fn fs_main(in : VsOut) -> @location(0) vec4<f32> {\n");
    /* Memory color: sampled up-front (uniform control flow; textureSampleLevel
     * so it stays valid regardless of later discards). */
    if (rdp_mem) {
        P("  let snapSize = vec2<f32>(textureDimensions(uSnap));\n");
        P("  let memoryUv = clamp(in.clip_pos.xy / snapSize, vec2<f32>(0.0), vec2<f32>(1.0));\n");
        P("  let memoryColor = textureSampleLevel(uSnap, uSnapSamp, memoryUv, 0.0);\n");
    }
    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) continue;
        const char *tex = i == 0 ? "uTex0" : "uTex1";
        const char *samp = i == 0 ? "uSamp0" : "uSamp1";
        bool clamp_s = cc.clamp[i][0], clamp_t = cc.clamp[i][1];
        bool mask_s = cc.tile_mask[i][0], mask_t = cc.tile_mask[i][1];
        bool filt = cc.n64_filter[i] && g_pcTextureAnisotropy <= 1;
        /* texSize when clamp/mask/filter needs it (textureDimensions). */
        if (clamp_s || clamp_t || mask_s || mask_t || filt) {
            P("  let texSize%d = vec2<f32>(textureDimensions(%s));\n", i, tex);
        }
        /* Shader-side UV clamp to the live tile window (gfx_opengl.c :1189-1203). */
        P("  var stc%d = in.vTexCoord%d;\n", i, i);
        if (clamp_s && clamp_t) {
            P("  stc%d = clamp(in.vTexCoord%d, vec2<f32>(0.5) / texSize%d, vec2<f32>(in.vTexClampS%d, in.vTexClampT%d));\n",
              i, i, i, i, i);
        } else if (clamp_s) {
            P("  stc%d.x = clamp(in.vTexCoord%d.x, 0.5 / texSize%d.x, in.vTexClampS%d);\n", i, i, i, i);
        } else if (clamp_t) {
            P("  stc%d.y = clamp(in.vTexCoord%d.y, 0.5 / texSize%d.y, in.vTexClampT%d);\n", i, i, i, i);
        }
        /* Sample: N64 3-point filter if the tile requests it, else mask-wrapped
         * or straight bilinear. */
        const char *ms = mask_s ? (i == 0 ? "in.vTexMaskS0" : "in.vTexMaskS1") : "0.0";
        const char *mt = mask_t ? (i == 0 ? "in.vTexMaskT0" : "in.vTexMaskT1") : "0.0";
        if (filt) {
            P("  let texVal%d : vec4<f32> = n64Filter3(%s, %s, stc%d, texSize%d, %s, %s);\n",
              i, tex, samp, i, i, ms, mt);
        } else if (mask_s || mask_t) {
            P("  let texVal%d : vec4<f32> = textureSample(%s, %s, n64TileMaskUv(stc%d, texSize%d, %s, %s));\n",
              i, tex, samp, i, i, ms, mt);
        } else {
            P("  let texVal%d : vec4<f32> = textureSample(%s, %s, stc%d);\n", i, tex, samp, i);
        }
    }

    P("  var texel : vec4<f32>;\n");
    int num_cycles = cc.opt_2cyc ? 2 : 1;
    for (int cyc = 0; cyc < num_cycles; cyc++) {
        if (!cc.color_alpha_same[cyc] && cc.opt_alpha) {
            P("  texel = vec4<f32>(");
            wgsl_formula(out, MODULE_CAP, &n, cc.c[cyc],
                         cc.do_single[cyc][0], cc.do_multiply[cyc][0], cc.do_mix[cyc][0],
                         false, false, true);
            P(", ");
            wgsl_formula(out, MODULE_CAP, &n, cc.c[cyc],
                         cc.do_single[cyc][1], cc.do_multiply[cyc][1], cc.do_mix[cyc][1],
                         true, true, true);
            P(");\n");
        } else if (cc.opt_alpha) {
            P("  texel = ");
            wgsl_formula(out, MODULE_CAP, &n, cc.c[cyc],
                         cc.do_single[cyc][0], cc.do_multiply[cyc][0], cc.do_mix[cyc][0],
                         true, false, true);
            P(";\n");
        } else {
            /* vec3 combiner: build the color, alpha forced to 1.0. */
            P("  texel = vec4<f32>(");
            wgsl_formula(out, MODULE_CAP, &n, cc.c[cyc],
                         cc.do_single[cyc][0], cc.do_multiply[cyc][0], cc.do_mix[cyc][0],
                         false, false, false);
            P(", 1.0);\n");
        }
        if (cyc == 0 && num_cycles == 2) {
            P("  texel = clamp(texel, vec4<f32>(-1.01), vec4<f32>(1.01));\n");
        }
    }
    P("  texel = clamp(texel, vec4<f32>(0.0), vec4<f32>(1.0));\n");

    if (cc.opt_dfdx_light) {
        P("  let smoothN = normalize(in.vSmoothNormal);\n");
        P("  let localSun = normalize(in.vLightDir);\n");
        P("  let ndl = max(dot(smoothN, localSun), 0.0);\n");
        P("  var litLinear = mdkrSrgbToLinear(texel.rgb);\n");
        P("  litLinear = litLinear * (vec3<f32>(1.0) + uLight.sunColourStrength.rgb * (uLight.sunColourStrength.a * ndl));\n");
        P("  texel = vec4<f32>(clamp(mdkrLinearToSrgb(litLinear), vec3<f32>(0.0), vec3<f32>(1.0)), texel.a);\n");
    }
    if (cc.opt_sun_shadow) {
        P("  var sunVisibility = mdkrShadowCascade(0, i32(uShadow.splitsLayers.z), in.vWorldPos);\n");
        P("  if (uShadow.params.w > 1.5) {\n");
        P("    let farVisibility = mdkrShadowCascade(1, i32(uShadow.splitsLayers.w), in.vWorldPos);\n");
        P("    let transition = smoothstep(uShadow.splitsLayers.x, uShadow.splitsLayers.y, in.vShadowDepth);\n");
        P("    sunVisibility = mix(sunVisibility, farVisibility, transition);\n");
        P("  }\n");
        P("  texel = vec4<f32>(texel.rgb * mix(uShadow.params.z, 1.0, sunVisibility), texel.a);\n");
    }

    /* Fog mix (rgb toward fog, alpha preserved). */
    if (cc.opt_fog) {
        if (cc.opt_dfdx_light) {
            P("  texel = vec4<f32>(mdkrLinearToSrgb(mix(mdkrSrgbToLinear(texel.rgb), mdkrSrgbToLinear(in.vFog.rgb), in.vFog.a)), texel.a);\n");
        } else {
            P("  texel = vec4<f32>(mix(texel.rgb, in.vFog.rgb, in.vFog.a), texel.a);\n");
        }
    }
    /* Texture-edge alpha cutout. */
    if (cc.opt_texture_edge && cc.opt_alpha) {
        P("  if (texel.a > " GFX_TEXTURE_EDGE_ALPHA_THRESHOLD_SHADER
          ") { texel.a = 1.0; } else { discard; }\n");
    }
    /* AC-3: G_AC_DITHER alpha dissolve — byte-match gfx_opengl.c:1337. Multiplies
     * alpha by floor(noise+0.5), killing ~50% of pixels stochastically (the
     * PCL_SURF dissolve modes: watch-menu fade watch.c:2873, BG particle LUT
     * bg.c:3247). GL/Metal emit this; WGSL previously dropped it entirely.
     * wgpu_noise/uNoise exist because needs_noise is seeded with opt_noise above. */
    if (cc.opt_alpha && cc.opt_noise) {
        P("  texel.a *= floor(wgpu_noise(in.clip_pos.xy) + 0.5);\n");
    }
    /* DAM-R1c: room_water_alpha_suppress — GL/Metal force the shell alpha to 0
     * (gfx_opengl.c:1360; Frigate settex 655, not Dam). WGSL previously omitted
     * it, so a suppressed shell rendered at authored alpha on WebGPU. */
    if (cc.opt_alpha && cc.room_water_alpha_suppress) {
        P("  texel.a = 0.0;\n");
    }
    /* RDP memory-blend arms — byte-exact port of gfx_opengl.c :1373-1410.
     * HW blending is disabled for these draws (wgpu_blend_state returns opaque);
     * the shader performs the N64 blender math against the snapshot's memory
     * color, and (cvg variant) the 8-sample coverage accumulate/wrap against the
     * coverage stored in the framebuffer alpha channel. */
    if (info->diag_rdp_cvg_memory_blend) {
        P("  let fragGL = vec2<f32>(in.clip_pos.x, snapSize.y - in.clip_pos.y);\n");
        P("  let diagTri0 = in.vDiagTri01.xy;\n");
        P("  let diagTri1 = in.vDiagTri01.zw;\n");
        P("  let diagTri2 = in.vDiagTri2;\n");
        P("  var coverageCount = 0.0;\n");
        P("  coverageCount += diagCoverageSample(fragGL, vec2<f32>(-0.500, -0.375), diagTri0, diagTri1, diagTri2);\n");
        P("  coverageCount += diagCoverageSample(fragGL, vec2<f32>( 0.000, -0.375), diagTri0, diagTri1, diagTri2);\n");
        P("  coverageCount += diagCoverageSample(fragGL, vec2<f32>(-0.250, -0.125), diagTri0, diagTri1, diagTri2);\n");
        P("  coverageCount += diagCoverageSample(fragGL, vec2<f32>( 0.250, -0.125), diagTri0, diagTri1, diagTri2);\n");
        P("  coverageCount += diagCoverageSample(fragGL, vec2<f32>(-0.500,  0.125), diagTri0, diagTri1, diagTri2);\n");
        P("  coverageCount += diagCoverageSample(fragGL, vec2<f32>( 0.000,  0.125), diagTri0, diagTri1, diagTri2);\n");
        P("  coverageCount += diagCoverageSample(fragGL, vec2<f32>(-0.250,  0.375), diagTri0, diagTri1, diagTri2);\n");
        P("  coverageCount += diagCoverageSample(fragGL, vec2<f32>( 0.250,  0.375), diagTri0, diagTri1, diagTri2);\n");
        P("  if (coverageCount < 0.5) { discard; }\n");
        P("  let memoryCoverage = floor(floor(clamp(memoryColor.a, 0.0, 1.0) * 255.0 + 0.5) / 32.0);\n");
        P("  let coverageTotal = coverageCount + memoryCoverage;\n");
        P("  let coverageWrap = step(8.0, coverageTotal);\n");
        P("  let newCoverage = coverageTotal - 8.0 * floor(coverageTotal / 8.0);\n");
        P("  let newCoverageAlpha = (newCoverage * 32.0) / 255.0;\n");
        P("  let pixelAlphaByte = floor(clamp(texel.a, 0.0, 1.0) * 255.0 + 0.5);\n");
        P("  let a0 = floor(pixelAlphaByte / 8.0);\n");
        P("  let a1 = floor((255.0 - pixelAlphaByte) / 8.0);\n");
        P("  let pixelByte = floor(clamp(texel.rgb, vec3<f32>(0.0), vec3<f32>(1.0)) * 255.0 + vec3<f32>(0.5));\n");
        P("  let memoryByte = floor(clamp(memoryColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0)) * 255.0 + vec3<f32>(0.5));\n");
        P("  let blendedByte = floor((pixelByte * a0 + memoryByte * (a1 + 1.0)) / 32.0);\n");
        P("  let outByte = mix(memoryByte, blendedByte, coverageWrap);\n");
        P("  texel = vec4<f32>(clamp(outByte / 255.0, vec3<f32>(0.0), vec3<f32>(1.0)), newCoverageAlpha);\n");
    } else if (info->diag_rdp_memory_blend) {
        P("  let pixelAlphaByte = floor(clamp(texel.a, 0.0, 1.0) * 255.0 + 0.5);\n");
        P("  let a0 = floor(pixelAlphaByte / 8.0);\n");
        P("  let a1 = floor((255.0 - pixelAlphaByte) / 8.0);\n");
        P("  let pixelByte = floor(clamp(texel.rgb, vec3<f32>(0.0), vec3<f32>(1.0)) * 255.0 + vec3<f32>(0.5));\n");
        P("  let memoryByte = floor(clamp(memoryColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0)) * 255.0 + vec3<f32>(0.5));\n");
        P("  let blendedByte = floor((pixelByte * a0 + memoryByte * (a1 + 1.0)) / 32.0);\n");
        P("  texel = vec4<f32>(clamp(blendedByte / 255.0, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);\n");
    }
    if (!cc.opt_alpha) {
        P("  texel.a = 1.0;\n");
    }
    P("  return texel;\n}\n");

#undef P
#undef ATTR_IN
#undef VARY
    return out;
}

/* ------------------------------------------------------------------------
 * Output-VI-filter post-FX pass (fullscreen triangle).
 *
 * A faithful WGSL port of gfx_opengl.c's output-filter fragment shader
 * (gfx_opengl_ensure_output_filter_program, :3196-3447). The math — the FXAA
 * kernel, CAS sharpen weights, the filmic tonemap curve, the grade
 * (brightness/contrast/saturation/tint), gamma, vignette falloff, Bayer dither,
 * and RGB555 quantize — is transcribed line-for-line; only the surrounding GLSL
 * scaffolding differs. As in GL, FXAA and CAS sharpen sample the RAW scene
 * texture for their neighbour taps (`sampleDst` == a point fetch of uTex), so
 * this is a single uber-pass, not a chain. Effect order matches GL exactly:
 *   sample -> FXAA -> colorScale/bias -> sRGB decode -> [SSAO/bloom/
 *   brightness/contrast/saturation/tint/filmic] -> sRGB encode -> gamma ->
 *   vignette -> CAS sharpen -> dither -> RGB555.
 *
 * WebGPU renders the scene offscreen at the surface resolution, so src == dst
 * (filterMode 0 = bilinear sampleDst, exact at integer taps); the GL VI-filter
 * rescale/aspect-fit modes are a diag-only feature and are not ported. SSAO
 * (planar v1) samples the scene depth target (@binding(4), default-off via
 * Video.Ssao) and darkens in linear light. All texture reads use
 * textureSampleLevel so the in-branch FXAA/bloom/sharpen samples stay valid
 * under WGSL's uniformity rules. `fragPos` is @builtin(position) (top-left
 * origin, matching the scene texture layout and the CopyTextureToTexture present
 * it replaces), so no V-flip.
 *
 * Two KNOWN, deliberate divergences from GL (both sub-LSB / arguably-more-correct):
 * - Gamma: GL's runtime filter runs TWO chained passes (source -> low_tex with
 *   applyPost=0, then low_tex -> screen with applyPost=1 — gfx_opengl.c:3766/
 *   3791), and gamma sits OUTSIDE the applyPost gate, so a non-default
 *   Video.Gamma is applied TWICE on GL (pow(1/g) squared). This single pass
 *   applies it once — the setting's documented meaning. Identical at the default
 *   gamma 1.0.
 * - Bayer dither / RGB555 threshold: the 4x4 pattern indexes gl_FragCoord, whose
 *   Y origin is bottom-left on GL and top-left here, so the pattern is
 *   vertically mirrored. It is a +/-0.5-LSB ordered-dither offset — spatially
 *   uniform noise, no visible or measurable difference. */
const char *gfx_webgpu_postfx_wgsl(void) {
    return
    "struct Post {\n"
    "  srcSize : vec2<f32>, dstSize : vec2<f32>,\n"
    "  colorScale : f32, colorBias : f32, gamma : f32, saturation : f32,\n"
    "  contrast : f32, brightness : f32, vignette : f32, sharpen : f32,\n"
    "  bloomThreshold : f32, bloomIntensity : f32, levelSat : f32, levelCon : f32,\n"
    "  colorTint : vec3<f32>,\n"
    "  applyPost : i32, dither : i32, bloom : i32, fxaa : i32,\n"
    "  tonemap : i32, rgb555 : i32, linearFinish : i32, ssao : i32,\n"
    "  ssaoRadius : f32, ssaoIntensity : f32, ssaoAspect : f32, ssaoProjA : f32,\n"
    "  ssaoProjB : f32,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u : Post;\n"
    "@group(0) @binding(1) var uTex : texture_2d<f32>;\n"
    "@group(0) @binding(2) var uSampN : sampler;\n"   /* nearest + clamp (unused; kept so the bind group layout is stable) */
    "@group(0) @binding(3) var uSampL : sampler;\n"   /* linear + clamp (sampleDst + bloom — GL filterMode-0 is bilinear) */
    "@group(0) @binding(4) var uDepthTex : texture_depth_2d;\n"   /* scene depth (SSAO) */
    "@group(0) @binding(5) var uSampD : sampler;\n"   /* nearest + clamp, non-filtering (depth reads) */
    "struct VOut { @builtin(position) pos : vec4<f32> };\n"
    "@vertex fn vs_main(@builtin(vertex_index) vid : u32) -> VOut {\n"
    "  var kPos = array<vec2<f32>,3>(vec2<f32>(-1.0,-1.0), vec2<f32>(3.0,-1.0), vec2<f32>(-1.0,3.0));\n"
    "  var o : VOut;\n"
    "  o.pos = vec4<f32>(kPos[vid], 0.0, 1.0);\n"
    "  return o;\n}\n"
    "fn bayer4(idx : i32) -> f32 {\n"
    "  var b = array<f32,16>(\n"
    "     0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,\n"
    "    12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,\n"
    "     3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,\n"
    "    15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0);\n"
    "  return b[idx];\n}\n"
    /* sampleDst: GL's filterMode-0 path is sampleCpuBilinear (the runtime output
     * pass draws with filter_mode=0 — gfx_opengl.c:3766/3791), so use the LINEAR
     * clamp sampler: at src==dst the integer taps (centre, edge luma, CAS) land on
     * exact texel centres (bilinear fraction 0 -> exact texel), while FXAA's
     * fractional directional taps get the same bilinear blend GL computes. */
    "fn sampleDst(fc : vec2<f32>) -> vec4<f32> {\n"
    "  return textureSampleLevel(uTex, uSampL, fc / u.dstSize, 0.0);\n}\n"
    "fn fxLuma(c : vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.299, 0.587, 0.114)); }\n"
    "fn srgbToLinear(value : vec3<f32>) -> vec3<f32> {\n"
    "  let c = clamp(value, vec3<f32>(0.0), vec3<f32>(1.0));\n"
    "  return select(c / 12.92, pow((c + 0.055) / 1.055, vec3<f32>(2.4)),\n"
    "                c >= vec3<f32>(0.04045));\n"
    "}\n"
    "fn linearToSrgb(value : vec3<f32>) -> vec3<f32> {\n"
    "  let c = max(value, vec3<f32>(0.0));\n"
    "  return select(c * 12.92, 1.055 * pow(c, vec3<f32>(1.0 / 2.4)) - 0.055,\n"
    "                c >= vec3<f32>(0.0031308));\n"
    "}\n"
    "fn filmicFinish(c : vec3<f32>) -> vec3<f32> {\n"
    "  let shoulder = c * 1.18 / (vec3<f32>(1.0) + c * 0.18);\n"
    "  return mix(c, shoulder, 0.20);\n"
    "}\n"
    "fn fxaa(fc : vec2<f32>, rgbM : vec3<f32>) -> vec3<f32> {\n"
    "  let lM = fxLuma(rgbM);\n"
    "  let lN = fxLuma(sampleDst(fc + vec2<f32>( 0.0,-1.0)).rgb);\n"
    "  let lS = fxLuma(sampleDst(fc + vec2<f32>( 0.0, 1.0)).rgb);\n"
    "  let lW = fxLuma(sampleDst(fc + vec2<f32>(-1.0, 0.0)).rgb);\n"
    "  let lE = fxLuma(sampleDst(fc + vec2<f32>( 1.0, 0.0)).rgb);\n"
    "  let lNW = fxLuma(sampleDst(fc + vec2<f32>(-1.0,-1.0)).rgb);\n"
    "  let lNE = fxLuma(sampleDst(fc + vec2<f32>( 1.0,-1.0)).rgb);\n"
    "  let lSW = fxLuma(sampleDst(fc + vec2<f32>(-1.0, 1.0)).rgb);\n"
    "  let lSE = fxLuma(sampleDst(fc + vec2<f32>( 1.0, 1.0)).rgb);\n"
    "  let lMin = min(lM, min(min(lN,lS), min(lW,lE)));\n"
    "  let lMax = max(lM, max(max(lN,lS), max(lW,lE)));\n"
    "  let rng = lMax - lMin;\n"
    "  if (rng < max(0.0625, lMax * 0.125)) { return rgbM; }\n"
    "  var dir : vec2<f32>;\n"
    "  dir.x = -((lNW + lNE) - (lSW + lSE));\n"
    "  dir.y =  ((lNW + lSW) - (lNE + lSE));\n"
    "  let dirReduce = max((lNW+lNE+lSW+lSE) * 0.03125, 0.0078125);\n"
    "  let rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
    "  dir = clamp(dir * rcpDirMin, vec2<f32>(-8.0), vec2<f32>(8.0));\n"
    "  let rgbA = 0.5 * (\n"
    "      sampleDst(fc + dir * (1.0/3.0 - 0.5)).rgb +\n"
    "      sampleDst(fc + dir * (2.0/3.0 - 0.5)).rgb);\n"
    "  let rgbB = rgbA * 0.5 + 0.25 * (\n"
    "      sampleDst(fc + dir * -0.5).rgb +\n"
    "      sampleDst(fc + dir *  0.5).rgb);\n"
    "  let lB = fxLuma(rgbB);\n"
    "  if (lB < lMin || lB > lMax) { return rgbA; }\n"
    "  return rgbB;\n}\n"
    "fn casSharpen(fc : vec2<f32>, rgbC : vec3<f32>) -> vec3<f32> {\n"
    "  let n = sampleDst(fc + vec2<f32>( 0.0,-1.0)).rgb;\n"
    "  let s = sampleDst(fc + vec2<f32>( 0.0, 1.0)).rgb;\n"
    "  let w = sampleDst(fc + vec2<f32>(-1.0, 0.0)).rgb;\n"
    "  let e = sampleDst(fc + vec2<f32>( 1.0, 0.0)).rgb;\n"
    "  let mn = min(rgbC, min(min(n,s), min(w,e)));\n"
    "  let mx = max(rgbC, max(max(n,s), max(w,e)));\n"
    "  var amp = clamp(min(mn, 1.0 - mx) / max(mx, vec3<f32>(0.0001)), vec3<f32>(0.0), vec3<f32>(1.0));\n"
    "  amp = sqrt(amp);\n"
    "  let peak = -0.125 - 0.075 * u.sharpen;\n"
    "  let wgt = amp * peak;\n"
    "  let sum = rgbC + (n + s + w + e) * wgt;\n"
    "  let rcpW = 1.0 / (1.0 + 4.0 * wgt);\n"
    "  let outc = clamp(sum * rcpW, mn, mx);\n"
    "  return mix(rgbC, outc, clamp(u.sharpen, 0.0, 1.0));\n}\n"
    /* Screen-space ambient occlusion from the scene depth texture — a term-for-term
     * port of gfx_opengl.c:3346-3374 (planar v1). Works on raw (non-linear) window
     * depth: a neighbour nearer than the centre by a scale-invariant margin is a
     * contact occluder. ssaoLinZ reuses GL's 2d-1 window->NDC mapping verbatim; the
     * depth read uses textureSampleLevel (explicit LOD, no derivative uniformity
     * concern under WGSL's control-flow rules). Depth24Plus samples as the stored
     * window-depth in [0,1], identical to GL's texture(uDepthTex,uv).r. */
    "fn ssaoLinZ(d : f32) -> f32 { return u.ssaoProjB / (u.ssaoProjA + 2.0 * d - 1.0); }\n"
    "fn ssaoAO(uv : vec2<f32>) -> f32 {\n"
    "  var kSsaoDir = array<vec2<f32>,8>(\n"
    "    vec2<f32>( 1.0, 0.0), vec2<f32>( 0.7071, 0.7071), vec2<f32>(0.0, 1.0), vec2<f32>(-0.7071, 0.7071),\n"
    "    vec2<f32>(-1.0, 0.0), vec2<f32>(-0.7071,-0.7071), vec2<f32>(0.0,-1.0), vec2<f32>( 0.7071,-0.7071));\n"
    "  let cd = textureSampleLevel(uDepthTex, uSampD, uv, 0);\n"
    "  if (cd >= 0.99999) { return 0.0; }\n"
    "  let cz = ssaoLinZ(cd);\n"
    "  var occ = 0.0;\n"
    "  for (var i = 0; i < 8; i = i + 1) {\n"
    "    var dir = kSsaoDir[i];\n"
    "    dir.x = dir.x / max(u.ssaoAspect, 0.001);\n"
    "    for (var sp = 1; sp <= 2; sp = sp + 1) {\n"
    "      let o = dir * u.ssaoRadius * f32(sp);\n"
    "      let nz = ssaoLinZ(textureSampleLevel(uDepthTex, uSampD, uv + o, 0));\n"
    "      let diff = cz - nz;\n"
    "      if (diff > cz * 0.015 && diff < cz * 0.12) { occ = occ + 1.0 / f32(sp); }\n"
    "    }\n"
    "  }\n"
    "  return occ / 12.0;\n}\n"
    "@fragment fn fs_main(in : VOut) -> @location(0) vec4<f32> {\n"
    "  let fc = in.pos.xy;\n"
    "  let uv = fc / u.dstSize;\n"
    "  var color = sampleDst(fc);\n"
    "  if (u.applyPost == 1 && u.fxaa == 1) { color = vec4<f32>(fxaa(fc, color.rgb), color.a); }\n"
    "  var rgb = clamp(color.rgb * u.colorScale + vec3<f32>(u.colorBias / 255.0), vec3<f32>(0.0), vec3<f32>(1.0));\n"
    "  if (u.linearFinish == 1) {\n"
    "    rgb = srgbToLinear(rgb);\n"
    "    if (u.ssao == 1) {\n"
    "      let ao = 1.0 - u.ssaoIntensity * ssaoAO(uv);\n"
    "      rgb = rgb * clamp(ao, 0.0, 1.0);\n"
    "    }\n"
    "    if (u.bloom == 1) {\n"
    "      let texel = 1.0 / u.srcSize;\n"
    "      var bloom = vec3<f32>(0.0);\n"
    "      var wsum = 0.0;\n"
    "      for (var y = -3; y <= 3; y = y + 1) {\n"
    "      for (var x = -3; x <= 3; x = x + 1) {\n"
    "        let o = vec2<f32>(f32(x), f32(y)) * texel * 2.0;\n"
    "        let sp = srgbToLinear(textureSampleLevel(uTex, uSampL, uv + o, 0.0).rgb);\n"
    "        let l = dot(sp, vec3<f32>(0.2126, 0.7152, 0.0722));\n"
    "        let b = max(l - u.bloomThreshold, 0.0) / max(1.0 - u.bloomThreshold, 0.001);\n"
    "        let wv = exp(-f32(x*x + y*y) / 6.0);\n"
    "        bloom = bloom + sp * b * wv; wsum = wsum + wv;\n"
    "      }}\n"
    "      rgb = rgb + bloom / max(wsum, 0.001) * u.bloomIntensity;\n"
    "    }\n"
    "    rgb = rgb + vec3<f32>(u.brightness);\n"
    "    let con = u.contrast * u.levelCon;\n"
    "    rgb = (rgb - vec3<f32>(0.18)) * con + vec3<f32>(0.18);\n"
    "    let luma = dot(rgb, vec3<f32>(0.2126, 0.7152, 0.0722));\n"
    "    let sat = u.saturation * u.levelSat;\n"
    "    rgb = mix(vec3<f32>(luma), rgb, sat);\n"
    "    rgb = rgb * u.colorTint;\n"
    "    if (u.tonemap == 1) {\n"
    "      rgb = filmicFinish(max(rgb, vec3<f32>(0.0)));\n"
    "    }\n"
    "    rgb = linearToSrgb(clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0)));\n"
    "  }\n"
    "  rgb = pow(rgb, vec3<f32>(1.0 / max(u.gamma, 0.001)));\n"
    "  if (u.applyPost == 1 && u.vignette > 0.0) {\n"
    "    let vc = uv - vec2<f32>(0.5);\n"
    "    let d = dot(vc, vc) * 2.0;\n"
    "    let vig = 1.0 - u.vignette * smoothstep(0.3, 1.0, d);\n"
    "    rgb = rgb * vig;\n"
    "  }\n"
    "  if (u.applyPost == 1 && u.sharpen > 0.0) { rgb = casSharpen(fc, rgb); }\n"
    "  if (u.applyPost == 1 && u.dither == 1) {\n"
    "    let dp = vec2<i32>(fc) & vec2<i32>(3);\n"
    "    let t = bayer4(dp.y * 4 + dp.x) - 0.5;\n"
    "    rgb = clamp(rgb + vec3<f32>(t / 255.0), vec3<f32>(0.0), vec3<f32>(1.0));\n"
    "  }\n"
    "  if (u.rgb555 != 0) {\n"
    "    var threshold = 0.5;\n"
    "    if (u.rgb555 == 2) {\n"
    "      let dp = vec2<i32>(fc) & vec2<i32>(3);\n"
    "      threshold = threshold + bayer4(dp.y * 4 + dp.x) - 0.5;\n"
    "    }\n"
    "    rgb = clamp(floor(clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0)) * 31.0 + threshold) / 31.0, vec3<f32>(0.0), vec3<f32>(1.0));\n"
    "  }\n"
    "  return vec4<f32>(rgb, color.a);\n}\n";
}
