/*
 * gfx_metal.mm — Native Metal rendering backend for macOS (opt-in; GL default).
 *
 * Implements the fast3d GfxRenderingAPI vtable (from Emill/n64-fast3d-engine,
 * MIT — see gfx_rendering_api.h) using Apple Metal directly, so screen-space
 * effects (SSAO / depth reconstruction) run natively instead of through Apple's
 * deprecated OpenGL-over-Metal translator, which op-hangs on that math.
 *
 * Structural reference: the libultraship (Kenix3, MIT) Metal backend. This file
 * is authored fresh against this port's 22-fn vtable; only the idioms are shared.
 * The combiner->MSL translator retargets THIS port's own GLSL string-builder
 * (gfx_opengl.c:722-1449) to emit MSL, with LUS p_shader_item_to_str as a 1:1
 * correctness oracle.
 *
 * Implemented (Phases 1-3): device/queue/CAMetalLayer bring-up + clear/present;
 * combiner -> MSL translation + per-combiner MTLLibrary/MTLRenderPipelineState;
 * offscreen scene targets (color + Depth32Float); texture upload; blend/depth/
 * sampler baked into cached PSOs; deferred-state draw flush over a ring vertex
 * arena; the RDP-memory / coverage-memory XLU framebuffer snapshot-and-resume;
 * and synchronous CPU readback (screenshots/probes).
 * The Phase 4/5 SSAO + output-VI-filter hook builds on the offscreen scene
 * color/depth targets and the snapshot machinery (see mtl_ensure_targets /
 * mtl_open_scene_encoder below).
 * ARC-managed (CMake sets -fobjc-arc for this TU).
 */

/* This project ships include/math.h — an N64 native-port shim (guard
 * _MATH_EXT_H_) that sits on the -I path ahead of the SDK and therefore SHADOWS
 * the system <math.h>. It lacks float_t/double_t, so the legacy Carbon <fp.h>
 * pulled in transitively by the Metal/QuartzCore umbrellas fails to compile
 * ("unknown type name 'double_t'"). fp.h is deprecated ("Use math.h instead")
 * and nothing in the Metal path needs it, so we suppress it via its own include
 * guard — LOCAL to this TU, so the shared shim (and the byte-identical C TUs)
 * are untouched. */
#define __FP__  /* skip <CoreServices/.../CarbonCore/fp.h> */

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dispatch/dispatch.h>
#include <os/signpost.h>      /* W3.E1: named GPU intervals for Instruments */

/* gfx_cc.c / gfx_rendering_api are C TUs — force C linkage so the mangled
 * references from this ObjC++ TU resolve to the unmangled C symbols. */
extern "C" {
#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"
#include "gfx_uniforms.h"   /* render/post-FX uniform state shared with the GL backend */
#include "gfx_msaa_util.h"  /* FID-0018: MSAA sample-count resolution (pure, unit-tested) */
/* SMAA (W3.E4) committed reference LUTs (E4.T1): AreaTex 160x560 RG8, SearchTex
 * 64x16 R8 — first-party generated from the MIT SMAA reference (no ROM data). */
#include "smaa_area_tex.h"
#include "smaa_search_tex.h"
}
/* port_env.h carries its own extern "C" guard (and pulls <stdio.h>), so include
 * it outside the C-linkage block above. */
#include "port_env.h"

/* Growable text buffer. STL (<string>/<vector>) is unusable in this TU because
 * the project's include/math.h shim shadows the system <math.h>, which breaks
 * libc++'s <cmath> (pulled transitively by <string>). So the shader-source
 * builder uses C buffers, exactly like the GL backend (gfx_opengl.c). */
struct TB {
    char *p;
    size_t len;
    size_t cap;
};
static void tb_init(TB *b, size_t cap) {
    b->p = (char *)malloc(cap);
    b->len = 0;
    /* cap stays 0 on OOM so tb_str/tb_fmt no-op (their `len+n+1 > cap` guard
     * rejects everything) rather than dereferencing a NULL buffer; the empty
     * source then routes into the controlled MSL-compile abort. */
    b->cap = b->p ? cap : 0;
    if (b->p) b->p[0] = '\0';
}
static void tb_free(TB *b) { free(b->p); b->p = NULL; }
static void tb_str(TB *b, const char *s) {
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) return; /* generous caps; never expected to hit */
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}
static void tb_fmt(TB *b, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, a);
    va_end(a);
    if (n > 0 && (size_t)n < b->cap - b->len) b->len += (size_t)n;
}

extern "C" void *platformGetMetalLayer(void);  /* platform_sdl.c */
extern "C" void minimap_overlay_draw_queued_frames_metal(int fb_width, int fb_height);

/* Set once by whichever backend inits; the CPU clipper + shader gen read it.
 * Metal init forces it true (native depth-clamp), so the z*=0.3 hack is dropped
 * and the CPU vertex/clip pipeline is byte-identical to GL. */
extern "C" {
    extern bool g_depth_clamp_enabled;
    /* Minimal re-declarations (avoid pulling gfx_pc.h -> ultra64.h into this TU).
     * Layout must match src/platform/gfx_pc.h and src/vi.h. */
    struct GfxDimensions { uint32_t width, height; float aspect_ratio; };
    extern struct GfxDimensions gfx_current_dimensions;
    short viGetX(void);
    short viGetY(void);
    /* Render/post-FX uniform state (g_pc*) is declared once in gfx_uniforms.h,
     * included above and shared with the GL backend. */
}

/* N64 tile wrap flags (PR/gbi.h) — declared locally to avoid the N64 header. */
#define GE007_G_TX_MIRROR 0x1
#define GE007_G_TX_CLAMP  0x2

/* Device/queue/layer live for the process; per-frame drawable & command buffer
 * are strong statics so ARC keeps them alive across the start_frame/end_frame
 * boundary (they are created in start_frame and presented in end_frame). */
static id<MTLDevice>         s_device   = nil;
static id<MTLCommandQueue>   s_queue    = nil;
static CAMetalLayer         *s_layer    = nil;
static double s_clear_r = 0.0, s_clear_g = 0.0, s_clear_b = 0.0;
static id<CAMetalDrawable>   s_drawable = nil;
static id<MTLCommandBuffer>  s_cmdbuf   = nil;
static bool s_logged_first_frame = false;

/* ==========================================================================
 * Phase 2 — combiner -> MSL shader translation
 * ========================================================================== */

/* One vertex attribute in the interleaved buf_vbo layout. size/offset in floats.
 * The order these are pushed mirrors gfx_pc.c:18390-18540 == the GL generator's
 * glGetAttribLocation push order (gfx_opengl.c:1349-1397), so the Metal vertex
 * descriptor and the frontend's VBO packing cannot drift. */
struct MtlAttr {
    int index;   /* [[attribute(index)]] */
    int size;    /* 1, 2, 3, or 4 floats */
    int offset;  /* float offset within the vertex */
};

struct MetalShader {
    uint64_t id0;
    uint32_t id1;
    struct CCFeatures cc;
    __strong id<MTLLibrary> library;
    __strong id<MTLFunction> vtxFn;
    __strong id<MTLFunction> fragFn;
    __strong MTLVertexDescriptor *vtxDesc;   /* built lazily in Phase 2.2 */
    __strong NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *psoCache; /* 2.2 */
    int numFloats;
    int numInputs;
    bool usedTextures[2];
    bool diagRdpMemory;
    bool diagRdpCvgMemory;
    MtlAttr attrs[32];
    int numAttrs;
};

/* Heap-allocated, never moved — ARC manages the __strong members. The pointer
 * array grows on demand (realloc) so lookups always hit and no compiled shader
 * is ever dropped/recompiled (unlike a fixed cap). Lives for the process. */
static MetalShader **s_shaders = nullptr;
static int s_shader_count = 0;
static int s_shader_cap = 0;
static MetalShader *s_cur_shader = nullptr;

/* Diagnostic MSL dump (mirrors GL's [SHADER_n] dump at gfx_opengl.c:1290). */
static int s_shader_dump_checked = -1;
static bool mtl_dump_shaders(void) {
    if (s_shader_dump_checked < 0) {
        s_shader_dump_checked = getenv("GE007_METAL_DUMP_SHADERS") ? 1 : 0;
    }
    return s_shader_dump_checked > 0;
}

/* MSL vocabulary port of shader_item_to_str (gfx_opengl.c:731-790). Same case
 * structure; GLSL -> MSL: vec4->float4, texture varyings referenced as locals
 * (aliased at fragment-body top), gl_FragCoord.xy -> fragCoord (GL-oriented,
 * flipped from Metal top-left), window_height -> u.winH, frame_count ->
 * u.frameCount. .rgb/.a swizzles are valid MSL. */
static const char *msl_item(uint32_t item, bool with_alpha, bool only_alpha,
                            bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:       return with_alpha ? "float4(0.0)" : "float3(0.0)";
            case SHADER_INPUT_1: return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2: return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3: return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4: return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_INPUT_5: return with_alpha || !inputs_have_alpha ? "vInput5" : "vInput5.rgb";
            case SHADER_INPUT_6: return with_alpha || !inputs_have_alpha ? "vInput6" : "vInput6.rgb";
            case SHADER_INPUT_7: return with_alpha || !inputs_have_alpha ? "vInput7" : "vInput7.rgb";
            case SHADER_TEXEL0:  return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A: return hint_single_element ? "texVal0.a" :
                (with_alpha ? "float4(texVal0.a)" : "float3(texVal0.a)");
            case SHADER_TEXEL1:  return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_TEXEL1A: return hint_single_element ? "texVal1.a" :
                (with_alpha ? "float4(texVal1.a)" : "float3(texVal1.a)");
            case SHADER_1:       return with_alpha ? "float4(1.0)" : "float3(1.0)";
            case SHADER_COMBINED: return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:   return with_alpha ?
                "float4(random(float3(floor(fragCoord * (240.0 / float(u.winH))), float(u.frameCount))))" :
                "float3(random(float3(floor(fragCoord * (240.0 / float(u.winH))), float(u.frameCount))))";
        }
    } else {
        switch (item) {
            case SHADER_0:       return "0.0";
            case SHADER_INPUT_1: return "vInput1.a";
            case SHADER_INPUT_2: return "vInput2.a";
            case SHADER_INPUT_3: return "vInput3.a";
            case SHADER_INPUT_4: return "vInput4.a";
            case SHADER_INPUT_5: return "vInput5.a";
            case SHADER_INPUT_6: return "vInput6.a";
            case SHADER_INPUT_7: return "vInput7.a";
            case SHADER_TEXEL0:  return "texVal0.a";
            case SHADER_TEXEL0A: return "texVal0.a";
            case SHADER_TEXEL1:  return "texVal1.a";
            case SHADER_TEXEL1A: return "texVal1.a";
            case SHADER_1:       return "1.0";
            case SHADER_COMBINED: return "texel.a";
            case SHADER_NOISE:   return "random(float3(floor(fragCoord * (240.0 / float(u.winH))), float(u.frameCount)))";
        }
    }
    return "0.0";
}

/* MSL port of append_formula (gfx_opengl.c:792-817) — identical arithmetic. */
static void msl_formula(TB *s, uint8_t c[2][4], bool do_single,
                        bool do_multiply, bool do_mix, bool with_alpha,
                        bool only_alpha, bool opt_alpha) {
    if (do_single) {
        tb_str(s, msl_item(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        tb_str(s, msl_item(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        tb_str(s, " * ");
        tb_str(s, msl_item(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        tb_str(s, "mix(");
        tb_str(s, msl_item(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        tb_str(s, ", ");
        tb_str(s, msl_item(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        tb_str(s, ", ");
        tb_str(s, msl_item(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        tb_str(s, ")");
    } else {
        tb_str(s, "(");
        tb_str(s, msl_item(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        tb_str(s, " - ");
        tb_str(s, msl_item(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        tb_str(s, ") * ");
        tb_str(s, msl_item(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        tb_str(s, " + ");
        tb_str(s, msl_item(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

/* W1.E2.T3 validation latch: GE007_WORLD_POS_DIAG makes fragmentMain visualize the
 * interpolated world position (fract(vWorldPos*0.01)) — mirrors the GL diag so the
 * pattern can be compared GL vs Metal. Run-constant. */
static int mtl_world_pos_diag_enabled(void) {
    static int v = -1;
    if (v < 0) v = (getenv("GE007_WORLD_POS_DIAG") != NULL);
    return v;
}

/* Build the full MSL translation unit (vertexMain + fragmentMain) for a
 * combiner into `out`. Mirrors gfx_opengl_create_and_load_new_shader:823-1284
 * block for block. Also fills ms->attrs / numFloats. */
static void mtl_generate_msl(MetalShader *ms, TB *s) {
    struct CCFeatures &cc = ms->cc;

    const char *input_interp  = cc.noperspective_inputs ? "[[center_no_perspective]] " : "";
    const char *texc_interp   = cc.noperspective_texcoords ? "[[center_no_perspective]] " : "";
    const char *fog_interp    = cc.noperspective_fog ? "[[center_no_perspective]] " : "";
    bool uses_tile_mask =
        cc.tile_mask[0][0] || cc.tile_mask[0][1] ||
        cc.tile_mask[1][0] || cc.tile_mask[1][1];

    bool needs_noise = cc.opt_noise;
    for (int ci = 0; ci < 2 && !needs_noise; ci++)
        for (int cj = 0; cj < 2 && !needs_noise; cj++)
            for (int ck = 0; ck < 4 && !needs_noise; ck++)
                if (cc.c[ci][cj][ck] == SHADER_NOISE) needs_noise = true;

    bool uses_fragcoord = needs_noise ||
        (cc.opt_alpha && (cc.diag_rdp_memory_blend || cc.diag_rdp_cvg_memory_blend)) ||
        (cc.opt_alpha && cc.opt_noise) ||
        (cc.opt_alpha && cc.diag_xlu_coverage_wrap_thin);

    /* ---- header ---- */
    tb_str(s, "#include <metal_stdlib>\n");
    tb_str(s, "using namespace metal;\n");

    /* Uniform block — layout must match the C Uniforms struct uploaded in 3.1.
     * Ordered largest-alignment-first for a clean 16-byte stride. */
    tb_str(s, "struct Uniforms {\n");
    tb_str(s, "    float4 diagViewport;\n");   /* offset 0  */
    tb_str(s, "    float2 n64FilterScale;\n"); /* offset 16 */
    tb_str(s, "    float2 diagFbOrigin;\n");   /* offset 24 */
    tb_str(s, "    int frameCount;\n");        /* offset 32 */
    tb_str(s, "    int winH;\n");              /* offset 36 (viewport height, noise scale) */
    tb_str(s, "    int fbH;\n");               /* offset 40 (attachment height, fragcoord flip) */
    tb_str(s, "    int _pad;\n");              /* offset 44 -> 48 */
    /* W1.E3.T4: sun-shadow receiver uniforms (must match struct MtlUniforms). */
    tb_str(s, "    float4x4 shadowMat;\n");    /* offset 48 -> 112 (16-aligned) */
    tb_str(s, "    float2 shadowTexel;\n");    /* offset 112 -> 120 */
    tb_str(s, "    float shadowBias;\n");      /* offset 120 -> 124 */
    tb_str(s, "    float shadowUmbra;\n");     /* offset 124 -> 128 */
    /* RL-5 linear-light sun (always present for one stable uniform layout). */
    tb_str(s, "    float4 sunColorLinear;\n"); /* offset 128 -> 144 */
    tb_str(s, "    float sunStrength;\n");      /* offset 144 */
    tb_str(s, "    float _lightPad0;\n");       /* offset 148 */
    tb_str(s, "    float _lightPad1;\n");       /* offset 152 */
    tb_str(s, "    float _lightPad2;\n");       /* offset 156 -> size 160 */
    tb_str(s, "};\n");

    /* ---- helper functions (file scope, dependency order) ---- */
    tb_str(s, "static float glslMod(float x, float y) { return x - y * floor(x / y); }\n");
    if (cc.opt_dfdx_light) {
        tb_str(s, "static float3 mdkrSrgbToLinear(float3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, float3(2.4)), step(float3(0.04045), c)); }\n");
        tb_str(s, "static float3 mdkrLinearToSrgb(float3 c) { c = max(c, float3(0.0)); return mix(c * 12.92, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, step(float3(0.0031308), c)); }\n");
    }

    if (uses_tile_mask || cc.n64_filter[0] || cc.n64_filter[1]) {
        tb_str(s, "static float n64TileMaskAxis(float texelCoord, float maskPeriod) {\n");
        tb_str(s, "    float extent = abs(maskPeriod);\n");
        tb_str(s, "    if (extent <= 0.5) return texelCoord;\n");
        tb_str(s, "    float coord = glslMod(texelCoord, maskPeriod < 0.0 ? extent * 2.0 : extent);\n");
        tb_str(s, "    if (maskPeriod < 0.0 && coord >= extent) coord = extent * 2.0 - coord;\n");
        tb_str(s, "    return coord;\n");
        tb_str(s, "}\n");
        tb_str(s, "static float2 n64TileMaskUv(float2 uv, float2 texSize, float maskS, float maskT) {\n");
        tb_str(s, "    float2 texelCoord = uv * texSize;\n");
        tb_str(s, "    texelCoord.x = n64TileMaskAxis(texelCoord.x, maskS);\n");
        tb_str(s, "    texelCoord.y = n64TileMaskAxis(texelCoord.y, maskT);\n");
        tb_str(s, "    return texelCoord / texSize;\n");
        tb_str(s, "}\n");
    }

    if (cc.n64_filter[0] || cc.n64_filter[1]) {
        bool always_3point = cc.n64_filter_always_3point;
        /* nearest_threshold: GL derives this from diag env knobs
         * (gfx_diag_n64_filter_nearest_threshold). Default is 1.0 for all live
         * paths; the env overrides are diag-only and out of Metal scope. */
        float nearest_threshold = 1.0f;
        tb_str(s, "static float4 n64TextureFilter(texture2d<float> tex, sampler smp, float2 uv, float maskS, float maskT, float2 filterScale) {\n");
        tb_str(s, "    float2 texSize = float2(tex.get_width(), tex.get_height());\n");
        tb_str(s, "    float2 texelCoord = uv * texSize;\n");
        if (!always_3point) {
            tb_str(s, "    float2 dx = dfdx(texelCoord) * filterScale.x;\n");
            tb_str(s, "    float2 dy = dfdy(texelCoord) * filterScale.y;\n");
            tb_str(s, "    float2 footprint = max(abs(dx), abs(dy));\n");
            tb_fmt(s, "    if (max(footprint.x, footprint.y) < %.9f) {\n", nearest_threshold);
            tb_str(s, "        return tex.sample(smp, n64TileMaskUv((floor(texelCoord) + float2(0.5)) / texSize, texSize, maskS, maskT), level(0.0));\n");
            tb_str(s, "    }\n");
        }
        tb_str(s, "    float2 offset = fract(uv * texSize - float2(0.5));\n");
        tb_str(s, "    offset -= step(1.0, offset.x + offset.y);\n");
        tb_str(s, "    float2 baseUv = uv - offset / texSize;\n");
        tb_str(s, "    float4 c0 = tex.sample(smp, n64TileMaskUv(baseUv, texSize, maskS, maskT), level(0.0));\n");
        tb_str(s, "    float4 c1 = tex.sample(smp, n64TileMaskUv(baseUv + float2(sign(offset.x), 0.0) / texSize, texSize, maskS, maskT), level(0.0));\n");
        tb_str(s, "    float4 c2 = tex.sample(smp, n64TileMaskUv(baseUv + float2(0.0, sign(offset.y)) / texSize, texSize, maskS, maskT), level(0.0));\n");
        tb_str(s, "    return c0 + abs(offset.x) * (c1 - c0) + abs(offset.y) * (c2 - c0);\n");
        tb_str(s, "}\n");
    }

    if (needs_noise) {
        tb_str(s, "static float random(float3 value) {\n");
        tb_str(s, "    float r = dot(sin(value), float3(12.9898, 78.233, 37.719));\n");
        tb_str(s, "    return fract(sin(r) * 143758.5453);\n");
        tb_str(s, "}\n");
    }

    if (cc.opt_alpha && cc.diag_rdp_cvg_memory_blend) {
        tb_str(s, "static float diagEdge(float2 a, float2 b, float2 p) {\n");
        tb_str(s, "    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);\n");
        tb_str(s, "}\n");
        tb_str(s, "static float diagInsideTri(float2 p, float2 a, float2 b, float2 c) {\n");
        tb_str(s, "    float e0 = diagEdge(a, b, p);\n");
        tb_str(s, "    float e1 = diagEdge(b, c, p);\n");
        tb_str(s, "    float e2 = diagEdge(c, a, p);\n");
        tb_str(s, "    bool hasNeg = (e0 < 0.0) || (e1 < 0.0) || (e2 < 0.0);\n");
        tb_str(s, "    bool hasPos = (e0 > 0.0) || (e1 > 0.0) || (e2 > 0.0);\n");
        tb_str(s, "    return (hasNeg && hasPos) ? 0.0 : 1.0;\n");
        tb_str(s, "}\n");
        tb_str(s, "static float diagCoverageSample(float2 fragCoord, float4 diagViewport, float2 pixelOffset, float2 a, float2 b, float2 c) {\n");
        tb_str(s, "    float2 p = ((fragCoord + pixelOffset - diagViewport.xy) / diagViewport.zw) * 2.0 - 1.0;\n");
        tb_str(s, "    return diagInsideTri(p, a, b, c);\n");
        tb_str(s, "}\n");
    }

    /* ---- VertexIn / VertexOut structs + attribute walk ---- */
    ms->numAttrs = 0;
    int attr_idx = 0;
    int foff = 0;
    TB vin, vout, vbody;
    tb_init(&vin, 8192);
    tb_init(&vout, 8192);
    tb_init(&vbody, 8192);
    tb_str(&vin, "struct VertexIn {\n");
    tb_str(&vout, "struct VertexOut {\n    float4 position [[position]];\n");
    tb_str(&vbody, "vertex VertexOut vertexMain(VertexIn in [[stage_in]]) {\n    VertexOut out;\n");

    auto add_attr = [&](const char *type, const char *name, int size) {
        tb_fmt(&vin, "    %s %s [[attribute(%d)]];\n", type, name, attr_idx);
        if (ms->numAttrs < 32) {
            ms->attrs[ms->numAttrs].index = attr_idx;
            ms->attrs[ms->numAttrs].size = size;
            ms->attrs[ms->numAttrs].offset = foff;
            ms->numAttrs++;
        }
        attr_idx++;
        foff += size;
    };

    add_attr("float4", "aVtxPos", 4);
    if (cc.opt_alpha && cc.diag_rdp_cvg_memory_blend) {
        add_attr("float4", "aDiagTri01", 4);
        add_attr("float2", "aDiagTri2", 2);
        tb_str(&vout, "    float4 vDiagTri01 [[center_no_perspective]];\n");
        tb_str(&vout, "    float2 vDiagTri2 [[center_no_perspective]];\n");
        tb_str(&vbody, "    out.vDiagTri01 = in.aDiagTri01;\n");
        tb_str(&vbody, "    out.vDiagTri2 = in.aDiagTri2;\n");
    }
    /* W1.E2.T3: world-space position attribute at the same ordinal as the VBO pack
     * (after pos/diag, before texcoords). Default perspective interpolation (no
     * [[center_no_perspective]]) — world position must interpolate perspective-correct. */
    if (cc.opt_world_pos) {
        add_attr("float3", "aWorldPos", 3);
        tb_str(&vout, "    float3 vWorldPos;\n");
        tb_str(&vbody, "    out.vWorldPos = in.aWorldPos;\n");
    }
    /* RL-5: smooth object normal + queued object-local sun direction. */
    if (cc.opt_dfdx_light) {
        add_attr("float3", "aSmoothNormal", 3);
        add_attr("float3", "aLightDir", 3);
        tb_str(&vout, "    float3 vSmoothNormal;\n");
        tb_str(&vout, "    float3 vLightDir;\n");
        tb_str(&vbody, "    out.vSmoothNormal = in.aSmoothNormal;\n");
        tb_str(&vbody, "    out.vLightDir = in.aLightDir;\n");
    }
    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) continue;
        char nm[24];
        snprintf(nm, sizeof nm, "aTexCoord%d", i); add_attr("float2", nm, 2);
        tb_fmt(&vout, "    %sfloat2 vTexCoord%d;\n", texc_interp, i);
        tb_fmt(&vbody, "    out.vTexCoord%d = in.aTexCoord%d;\n", i, i);
        for (int axis = 0; axis < 2; axis++) {
            char ax = axis == 0 ? 'S' : 'T';
            if (cc.clamp[i][axis]) {
                snprintf(nm, sizeof nm, "aTexClamp%c%d", ax, i); add_attr("float", nm, 1);
                tb_fmt(&vout, "    %sfloat vTexClamp%c%d;\n", texc_interp, ax, i);
                tb_fmt(&vbody, "    out.vTexClamp%c%d = in.aTexClamp%c%d;\n", ax, i, ax, i);
            }
            if (cc.tile_mask[i][axis]) {
                snprintf(nm, sizeof nm, "aTexMask%c%d", ax, i); add_attr("float", nm, 1);
                tb_fmt(&vout, "    %sfloat vTexMask%c%d;\n", texc_interp, ax, i);
                tb_fmt(&vbody, "    out.vTexMask%c%d = in.aTexMask%c%d;\n", ax, i, ax, i);
            }
        }
    }
    if (cc.opt_fog) {
        add_attr("float4", "aFog", 4);
        tb_fmt(&vout, "    %sfloat4 vFog;\n", fog_interp);
        tb_str(&vbody, "    out.vFog = in.aFog;\n");
    }
    for (int i = 0; i < cc.num_inputs; i++) {
        char nm[24];
        const char *ty = cc.opt_alpha ? "float4" : "float3";
        snprintf(nm, sizeof nm, "aInput%d", i + 1); add_attr(ty, nm, cc.opt_alpha ? 4 : 3);
        tb_fmt(&vout, "    %s%s vInput%d;\n", input_interp, ty, i + 1);
        tb_fmt(&vbody, "    out.vInput%d = in.aInput%d;\n", i + 1, i + 1);
    }
    tb_str(&vin, "};\n");
    tb_str(&vout, "};\n");
    /* g_depth_clamp_enabled is forced true on Metal, so the GL z*=0.3 spreading
     * hack is dropped: pass clip position through unchanged. */
    tb_str(&vbody, "    out.position = in.aVtxPos;\n    return out;\n}\n");

    ms->numFloats = foff;

    tb_str(s, vin.p);
    tb_str(s, vout.p);
    tb_str(s, vbody.p);
    tb_free(&vin);
    tb_free(&vout);
    tb_free(&vbody);

    /* ---- fragmentMain ---- */
    tb_str(s, "fragment float4 fragmentMain(VertexOut in [[stage_in]],\n");
    tb_str(s, "                            constant Uniforms& u [[buffer(1)]]");
    if (cc.used_textures[0])
        tb_str(s, ",\n                            texture2d<float> uTex0 [[texture(0)]], sampler smp0 [[sampler(0)]]");
    if (cc.used_textures[1])
        tb_str(s, ",\n                            texture2d<float> uTex1 [[texture(1)]], sampler smp1 [[sampler(1)]]");
    if (cc.opt_alpha && (cc.diag_rdp_memory_blend || cc.diag_rdp_cvg_memory_blend))
        tb_str(s, ",\n                            texture2d<float> uDiagFramebuffer [[texture(2)]], sampler smpDiag [[sampler(2)]]");
    if (cc.opt_sun_shadow)   /* W1.E3.T4: shadow map + comparison sampler on unit 5 (§4.4) */
        tb_str(s, ",\n                            depth2d<float> uShadowMap [[texture(5)]], sampler uShadowSmp [[sampler(5)]]");
    tb_str(s, ") {\n");

    /* Alias varyings into locals so the ported body reads exactly like the GLSL
     * (only vocabulary differs, not name-scoping). */
    for (int i = 0; i < cc.num_inputs; i++)
        tb_fmt(s, "    %s vInput%d = in.vInput%d;\n", cc.opt_alpha ? "float4" : "float3", i + 1, i + 1);
    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) continue;
        tb_fmt(s, "    float2 vTexCoord%d = in.vTexCoord%d;\n", i, i);
        for (int axis = 0; axis < 2; axis++) {
            char ax = axis == 0 ? 'S' : 'T';
            if (cc.clamp[i][axis])     tb_fmt(s, "    float vTexClamp%c%d = in.vTexClamp%c%d;\n", ax, i, ax, i);
            if (cc.tile_mask[i][axis]) tb_fmt(s, "    float vTexMask%c%d = in.vTexMask%c%d;\n", ax, i, ax, i);
        }
    }
    if (cc.opt_fog) tb_str(s, "    float4 vFog = in.vFog;\n");
    if (cc.opt_alpha && cc.diag_rdp_cvg_memory_blend) {
        tb_str(s, "    float4 vDiagTri01 = in.vDiagTri01;\n");
        tb_str(s, "    float2 vDiagTri2 = in.vDiagTri2;\n");
    }
    if (uses_fragcoord)
        tb_str(s, "    float2 fragCoord = float2(in.position.x, float(u.fbH) - in.position.y);\n");

    /* Shader-side UV clamping (gfx_opengl.c:1079-1107). .s/.t -> .x/.y. */
    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) continue;
        tb_fmt(s, "    float2 sampleTexCoord%d = vTexCoord%d;\n", i, i);
        if (cc.clamp[i][0] || cc.clamp[i][1] || cc.tile_mask[i][0] || cc.tile_mask[i][1])
            tb_fmt(s, "    float2 texSize%d = float2(uTex%d.get_width(), uTex%d.get_height());\n", i, i, i);
        if (cc.clamp[i][0] && cc.clamp[i][1]) {
            tb_fmt(s, "    sampleTexCoord%d = clamp(vTexCoord%d, 0.5 / texSize%d, float2(vTexClampS%d, vTexClampT%d));\n", i, i, i, i, i);
        } else if (cc.clamp[i][0]) {
            tb_fmt(s, "    sampleTexCoord%d.x = clamp(vTexCoord%d.x, 0.5 / texSize%d.x, vTexClampS%d);\n", i, i, i, i);
        } else if (cc.clamp[i][1]) {
            tb_fmt(s, "    sampleTexCoord%d.y = clamp(vTexCoord%d.y, 0.5 / texSize%d.y, vTexClampT%d);\n", i, i, i, i);
        }
    }

    for (int i = 0; i < 2; i++) {
        if (!cc.used_textures[i]) continue;
        const char *mask_s = cc.tile_mask[i][0] ? (i == 0 ? "vTexMaskS0" : "vTexMaskS1") : "0.0";
        const char *mask_t = cc.tile_mask[i][1] ? (i == 0 ? "vTexMaskT0" : "vTexMaskT1") : "0.0";
        const char *tx = i == 0 ? "uTex0" : "uTex1";
        const char *sm = i == 0 ? "smp0" : "smp1";
        if (cc.n64_filter[i]) {
            tb_fmt(s, "    float4 texVal%d = n64TextureFilter(%s, %s, sampleTexCoord%d, %s, %s, u.n64FilterScale);\n",
                   i, tx, sm, i, mask_s, mask_t);
        } else if (cc.tile_mask[i][0] || cc.tile_mask[i][1]) {
            tb_fmt(s, "    float4 texVal%d = %s.sample(%s, n64TileMaskUv(sampleTexCoord%d, texSize%d, %s, %s));\n",
                   i, tx, sm, i, i, mask_s, mask_t);
        } else {
            tb_fmt(s, "    float4 texVal%d = %s.sample(%s, sampleTexCoord%d);\n", i, tx, sm, i);
        }
    }

    if (cc.opt_alpha && cc.diag_alpha_from_tex_intensity) {
        /* GL mixes toward tex intensity with a diag knob (default 1.0). */
        float mix = 1.0f;
        if (cc.used_textures[0]) tb_fmt(s, "    texVal0.a = mix(texVal0.a, texVal0.r, %.9g);\n", mix);
        if (cc.used_textures[1]) tb_fmt(s, "    texVal1.a = mix(texVal1.a, texVal1.r, %.9g);\n", mix);
    }

    tb_str(s, cc.opt_alpha ? "    float4 texel;\n" : "    float3 texel;\n");
    int num_cycles = cc.opt_2cyc ? 2 : 1;
    for (int cyc = 0; cyc < num_cycles; cyc++) {
        tb_str(s, "    texel = ");
        if (!cc.color_alpha_same[cyc] && cc.opt_alpha) {
            tb_str(s, "float4(");
            msl_formula(s, cc.c[cyc], cc.do_single[cyc][0], cc.do_multiply[cyc][0], cc.do_mix[cyc][0], false, false, true);
            tb_str(s, ", ");
            msl_formula(s, cc.c[cyc], cc.do_single[cyc][1], cc.do_multiply[cyc][1], cc.do_mix[cyc][1], true, true, true);
            tb_str(s, ")");
        } else {
            msl_formula(s, cc.c[cyc], cc.do_single[cyc][0], cc.do_multiply[cyc][0], cc.do_mix[cyc][0],
                        cc.opt_alpha, false, cc.opt_alpha);
        }
        tb_str(s, ";\n");
        if (cyc == 0 && num_cycles == 2)
            tb_str(s, "    texel = clamp(texel, -1.01, 1.01);\n");
    }
    tb_str(s, "    texel = clamp(texel, 0.0, 1.0);\n");

    /* RL-5/CO-1: smooth-normal diffuse in linear RGB, baked result retained. */
    if (cc.opt_dfdx_light) {
        tb_str(s, "    float3 smoothN = normalize(in.vSmoothNormal);\n");
        tb_str(s, "    float3 localSun = normalize(in.vLightDir);\n");
        tb_str(s, "    float ndl = max(dot(smoothN, localSun), 0.0);\n");
        tb_str(s, "    float3 litLinear = mdkrSrgbToLinear(texel.rgb);\n");
        tb_str(s, "    litLinear *= float3(1.0) + u.sunColorLinear.xyz * (u.sunStrength * ndl);\n");
        tb_str(s, "    texel.rgb = clamp(mdkrLinearToSrgb(litLinear), 0.0, 1.0);\n");
    }

    /* W1.E3.T4: sun-shadow receiver — 3x3 PCF, after RL-5 lighting (so shadow
     * attenuation lands on top of the recomputed lighting and shadowed areas stay
     * dark) and before the fog mix (fog must win, §4.5). Mirrors the GLSL block;
     * sample_compare + a 1-suv.y flip for Metal's top-left texture origin. */
    if (cc.opt_sun_shadow) {
        tb_str(s, "    {\n");
        tb_str(s, "      float4 sc = u.shadowMat * float4(in.vWorldPos, 1.0);\n");
        tb_str(s, "      float3 suv = sc.xyz / sc.w * 0.5 + 0.5;\n");
        tb_str(s, "      suv.y = 1.0 - suv.y;\n");
        tb_str(s, "      float sh = 0.0;\n");
        tb_str(s, "      for (int dy = -1; dy <= 1; ++dy)\n");
        tb_str(s, "        for (int dx = -1; dx <= 1; ++dx)\n");
        tb_str(s, "          sh += uShadowMap.sample_compare(uShadowSmp, suv.xy + float2(dx, dy) * u.shadowTexel, suv.z - u.shadowBias);\n");
        tb_str(s, "      sh /= 9.0;\n");
        tb_str(s, "      if (any(abs(suv - 0.5) > float3(0.5))) sh = 1.0;\n");
        tb_str(s, "      texel.rgb *= mix(u.shadowUmbra, 1.0, sh);\n");
        tb_str(s, "    }\n");
    }

    if (cc.opt_fog) {
        if (cc.opt_dfdx_light)
            tb_str(s, "    texel.rgb = mdkrLinearToSrgb(mix(mdkrSrgbToLinear(texel.rgb), mdkrSrgbToLinear(vFog.rgb), vFog.a));\n");
        else if (cc.opt_alpha)
            tb_str(s, "    texel = float4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);\n");
        else
            tb_str(s, "    texel = mix(texel, vFog.rgb, vFog.a);\n");
    }

    if (cc.opt_texture_edge && cc.opt_alpha)
        tb_str(s, "    if (texel.a > 0.19) { texel.a = 1.0; } else { discard_fragment(); return float4(0.0); }\n");

    if (cc.opt_alpha && cc.opt_noise)
        tb_str(s, "    texel.a *= floor(random(float3(floor(fragCoord * (240.0 / float(u.winH))), float(u.frameCount))) + 0.5);\n");

    /* diag_color_scale / diag_alpha_scale: GL reads a settex knob (default ~1.0).
     * Live scale is 1.0 unless the diag env is set (out of Metal scope). */
    if (cc.diag_color_scale) {
        if (cc.opt_alpha) tb_str(s, "    texel = float4(clamp(texel.rgb * 1.02, 0.0, 1.0), texel.a);\n");
        else              tb_str(s, "    texel = clamp(texel * 1.02, 0.0, 1.0);\n");
    }
    if (cc.opt_alpha && cc.diag_alpha_scale)
        tb_str(s, "    texel.a = clamp(texel.a * 1.0, 0.0, 1.0);\n");
    if (cc.opt_alpha && cc.room_water_alpha_suppress)
        tb_str(s, "    texel.a = 0.0;\n");

    if (cc.opt_alpha && cc.diag_xlu_coverage_wrap_thin) {
        float rate = 0.25f;
        tb_fmt(s, "    float coverageWrapHash = fract(sin(dot(floor(fragCoord), float2(12.9898, 78.233))) * 43758.5453);\n"
                  "    if (coverageWrapHash >= %.9f) { discard_fragment(); return float4(0.0); }\n", rate);
    }

    if (cc.opt_alpha && cc.diag_rdp_cvg_memory_blend) {
        /* memoryUv samples the framebuffer SNAPSHOT (a direct top-left copy of
         * the scene color), so it uses Metal-native in.position (top-left) —
         * NOT the GL-oriented flipped fragCoord that coverage/noise use.
         * u.diagFbOrigin is the top-left viewport origin (Phase 3.3). */
        tb_str(s, "    float2 memoryUv = (in.position.xy - u.diagFbOrigin) / float2(uDiagFramebuffer.get_width(), uDiagFramebuffer.get_height());\n");
        tb_str(s, "    float4 memoryColor = uDiagFramebuffer.sample(smpDiag, clamp(memoryUv, float2(0.0), float2(1.0)));\n");
        tb_str(s, "    float2 diagTri0 = vDiagTri01.xy;\n");
        tb_str(s, "    float2 diagTri1 = vDiagTri01.zw;\n");
        tb_str(s, "    float2 diagTri2 = vDiagTri2;\n");
        tb_str(s, "    float coverageCount = 0.0;\n");
        const char *offs[8][2] = {
            {"-0.500", "-0.375"}, {" 0.000", "-0.375"}, {"-0.250", "-0.125"}, {" 0.250", "-0.125"},
            {"-0.500", " 0.125"}, {" 0.000", " 0.125"}, {"-0.250", " 0.375"}, {" 0.250", " 0.375"}};
        for (int k = 0; k < 8; k++)
            tb_fmt(s, "    coverageCount += diagCoverageSample(fragCoord, u.diagViewport, float2(%s, %s), diagTri0, diagTri1, diagTri2);\n", offs[k][0], offs[k][1]);
        tb_str(s, "    if (coverageCount < 0.5) { discard_fragment(); return float4(0.0); }\n");
        tb_str(s, "    float memoryCoverage = floor(floor(clamp(memoryColor.a, 0.0, 1.0) * 255.0 + 0.5) / 32.0);\n");
        tb_str(s, "    float coverageTotal = coverageCount + memoryCoverage;\n");
        tb_str(s, "    float coverageWrap = step(8.0, coverageTotal);\n");
        tb_str(s, "    float newCoverage = glslMod(coverageTotal, 8.0);\n");
        tb_str(s, "    float newCoverageAlpha = (newCoverage * 32.0) / 255.0;\n");
        tb_str(s, "    float pixelAlphaByte = floor(clamp(texel.a, 0.0, 1.0) * 255.0 + 0.5);\n");
        tb_str(s, "    float a0 = floor(pixelAlphaByte / 8.0);\n");
        tb_str(s, "    float a1 = floor((255.0 - pixelAlphaByte) / 8.0);\n");
        tb_str(s, "    float3 pixelByte = floor(clamp(texel.rgb, 0.0, 1.0) * 255.0 + 0.5);\n");
        tb_str(s, "    float3 memoryByte = floor(clamp(memoryColor.rgb, 0.0, 1.0) * 255.0 + 0.5);\n");
        tb_str(s, "    float3 blendedByte = floor((pixelByte * a0 + memoryByte * (a1 + 1.0)) / 32.0);\n");
        tb_str(s, "    float3 outByte = mix(memoryByte, blendedByte, coverageWrap);\n");
        tb_str(s, "    texel = float4(clamp(outByte / 255.0, 0.0, 1.0), newCoverageAlpha);\n");
    } else if (cc.opt_alpha && cc.diag_rdp_memory_blend) {
        /* memoryUv samples the framebuffer SNAPSHOT (a direct top-left copy of
         * the scene color), so it uses Metal-native in.position (top-left) —
         * NOT the GL-oriented flipped fragCoord that coverage/noise use.
         * u.diagFbOrigin is the top-left viewport origin (Phase 3.3). */
        tb_str(s, "    float2 memoryUv = (in.position.xy - u.diagFbOrigin) / float2(uDiagFramebuffer.get_width(), uDiagFramebuffer.get_height());\n");
        tb_str(s, "    float4 memoryColor = uDiagFramebuffer.sample(smpDiag, clamp(memoryUv, float2(0.0), float2(1.0)));\n");
        tb_str(s, "    float pixelAlphaByte = floor(clamp(texel.a, 0.0, 1.0) * 255.0 + 0.5);\n");
        tb_str(s, "    float a0 = floor(pixelAlphaByte / 8.0);\n");
        tb_str(s, "    float a1 = floor((255.0 - pixelAlphaByte) / 8.0);\n");
        tb_str(s, "    float3 pixelByte = floor(clamp(texel.rgb, 0.0, 1.0) * 255.0 + 0.5);\n");
        tb_str(s, "    float3 memoryByte = floor(clamp(memoryColor.rgb, 0.0, 1.0) * 255.0 + 0.5);\n");
        tb_str(s, "    float3 blendedByte = floor((pixelByte * a0 + memoryByte * (a1 + 1.0)) / 32.0);\n");
        tb_str(s, "    texel = float4(clamp(blendedByte / 255.0, 0.0, 1.0), 1.0);\n");
    }

    if (cc.opt_world_pos && mtl_world_pos_diag_enabled())
        tb_str(s, "    return float4(fract(in.vWorldPos * 0.01), 1.0);\n}\n");
    else if (cc.opt_alpha) tb_str(s, "    return texel;\n}\n");
    else                   tb_str(s, "    return float4(texel, 1.0);\n}\n");
}

static struct ShaderProgram *mtl_create_and_load_new_shader(uint64_t id0, uint32_t id1) {
    MetalShader *ms = new MetalShader();
    ms->id0 = id0;
    ms->id1 = id1;
    gfx_cc_get_features(id0, id1, &ms->cc);
    ms->numInputs = ms->cc.num_inputs;
    ms->usedTextures[0] = ms->cc.used_textures[0];
    ms->usedTextures[1] = ms->cc.used_textures[1];
    ms->diagRdpMemory = ms->cc.opt_alpha && ms->cc.diag_rdp_memory_blend;
    ms->diagRdpCvgMemory = ms->cc.opt_alpha && ms->cc.diag_rdp_cvg_memory_blend;

    TB src;
    tb_init(&src, 65536);
    mtl_generate_msl(ms, &src);

    if (mtl_dump_shaders()) {
        fprintf(stderr, "[metal] MSL for id0=0x%016llX id1=0x%X:\n%s\n--- END ---\n",
                (unsigned long long)id0, id1, src.p);
    }

  @autoreleasepool {  /* MSL compile (newLibraryWithSource) allocates a stack of
                       * Objective-C temporaries in the toolchain frontend; without
                       * a pool they live until this thread's next drain (M3.2). */
    NSError *err = nil;
    NSString *nssrc = [NSString stringWithUTF8String:src.p];
    id<MTLLibrary> lib = [s_device newLibraryWithSource:nssrc options:nil error:&err];
    if (lib == nil) {
        fprintf(stderr, "[metal] FATAL: MSL compile failed for id0=0x%016llX id1=0x%X:\n%s\nSource:\n%s\n",
                (unsigned long long)id0, id1,
                err ? err.localizedDescription.UTF8String : "(no error)", src.p);
        abort();
    }
    ms->library = lib;
    ms->vtxFn = [lib newFunctionWithName:@"vertexMain"];
    ms->fragFn = [lib newFunctionWithName:@"fragmentMain"];
    if (ms->vtxFn == nil || ms->fragFn == nil) {
        fprintf(stderr, "[metal] FATAL: missing vertexMain/fragmentMain for id0=0x%016llX\n",
                (unsigned long long)id0);
        abort();
    }
    ms->psoCache = [NSMutableDictionary dictionary];
  }
    tb_free(&src);

    if (s_shader_count >= s_shader_cap) {
        int newcap = s_shader_cap ? s_shader_cap * 2 : 256;
        MetalShader **grown = (MetalShader **)realloc(s_shaders, (size_t)newcap * sizeof(MetalShader *));
        if (grown != nullptr) {
            s_shaders = grown;
            s_shader_cap = newcap;
        }
    }
    if (s_shader_count < s_shader_cap) {
        s_shaders[s_shader_count++] = ms;
    } else {
        /* realloc failed (OOM) — the shader still works for this call; it just
         * won't be cached (may recompile). Prefer that over a crash. */
        fprintf(stderr, "[metal] WARNING: shader pool grow failed — not caching\n");
    }
    s_cur_shader = ms;
    return (struct ShaderProgram *)ms;
}

static struct ShaderProgram *mtl_lookup_shader(uint64_t id0, uint32_t id1) {
    for (int i = 0; i < s_shader_count; i++) {
        MetalShader *ms = s_shaders[i];
        if (ms->id0 == id0 && ms->id1 == id1) return (struct ShaderProgram *)ms;
    }
    return nullptr;
}

static void mtl_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    MetalShader *ms = (MetalShader *)prg;
    if (ms == nullptr) {
        if (num_inputs) *num_inputs = 0;
        if (used_textures) { used_textures[0] = used_textures[1] = false; }
        return;
    }
    if (num_inputs) *num_inputs = (uint8_t)ms->numInputs;
    if (used_textures) { used_textures[0] = ms->usedTextures[0]; used_textures[1] = ms->usedTextures[1]; }
}

static void mtl_load_shader(struct ShaderProgram *new_prg) {
    s_cur_shader = (MetalShader *)new_prg;
}

static void mtl_unload_shader(struct ShaderProgram *old_prg) {
    if (s_cur_shader == (MetalShader *)old_prg) s_cur_shader = nullptr;
}

/* ==========================================================================
 * Phase 3 — render targets, deferred state, caches, draw flush
 * ========================================================================== */

/* Uniform block — byte layout MUST match `struct Uniforms` in the generated MSL
 * (mtl_generate_msl). 16-byte aligned, 80 bytes total. */
struct MtlUniforms {
    float diagViewport[4];   /* 0  */
    float n64FilterScale[2]; /* 16 */
    float diagFbOrigin[2];   /* 24 */
    int   frameCount;        /* 32 */
    int   winH;              /* 36 */
    int   fbH;               /* 40 */
    int   _pad;              /* 44 -> 48 */
    /* W1.E3.T4: sun-shadow receiver uniforms — MUST mirror the MSL Uniforms struct. */
    float shadowMat[16];     /* 48 -> 112 (column-major, MSL float4x4) */
    float shadowTexel[2];    /* 112 -> 120 */
    float shadowBias;        /* 120 -> 124 */
    float shadowUmbra;       /* 124 -> 128 */
    /* RL-5 linear-light sun — MUST mirror generated MSL Uniforms. */
    float sunColorLinear[4]; /* 128 -> 144 */
    float sunStrength;       /* 144 */
    float _lightPad0;        /* 148 */
    float _lightPad1;        /* 152 */
    float _lightPad2;        /* 156 -> 160 */
};
static_assert(sizeof(struct MtlUniforms) == 160,
              "MtlUniforms must byte-match MSL (48 base + 80 shadow + 32 light)");

/* Offscreen scene targets (color is sampleable + blittable for readback and the
 * RDP snapshot; the drawable is framebufferOnly and cannot serve those). */
static id<MTLTexture> s_scene_color = nil;
static id<MTLTexture> s_scene_depth = nil;
/* FID-0018: multisample scene attachments, allocated only when Video.MSAA
 * resolves to >1 (else nil, and the scene pass binds the single-sample targets
 * above unchanged = byte-identical to pre-fix). The scene pass renders into
 * these and StoreAndMultisampleResolve's the color into s_scene_color (which the
 * snapshot copy, output filter, present blit and readback all read as before);
 * the MS depth is kept (Store) so a mid-frame snapshot reopen can Load it, and is
 * never sampled (SSAO is disabled under MSAA, matching the GL contract).
 * s_msaa_samples is the effective, device-clamped rasterSampleCount (1 = off). */
static id<MTLTexture> s_scene_color_ms = nil;
static id<MTLTexture> s_scene_depth_ms = nil;
static int s_msaa_samples = 1;
/* FID-0018: the MSAA sample count whose MS-texture allocation last failed at the
 * current framebuffer size (0 = none). Under sustained VRAM pressure this stops
 * mtl_ensure_targets from re-attempting the same doomed allocation — and respamming
 * the warning + reallocating every target — on every frame; it retries only when
 * the requested count or the size changes. */
static int s_msaa_failed_req = 0;
/* W1.E3.T3: sun shadow depth-only pass state (mirrors gfx_opengl.c). */
static id<MTLTexture> s_shadow_depth = nil;      /* Depth32Float, res x res */
static int s_shadow_res = 0;
static id<MTLRenderPipelineState> s_shadow_pso = nil;
static id<MTLDepthStencilState> s_shadow_ds = nil;
/* s_shadow_vbuf is ring-buffered [MTL_VBUF_SLOTS] (declared below, once
 * MTL_VBUF_SLOTS is defined) for the same reason as s_vbuf: up to
 * MTL_VBUF_SLOTS frames can be in flight, and an in-flight shadow pass
 * (mtl_shadow_encode) may still be reading a slot's buffer while the CPU
 * wants to memcpy new geometry into it (§3.4 fix). */
static id<MTLSamplerState> s_shadow_cmp_sampler = nil;  /* T4 receiver: LessEqual compare */
/* M3.2 fix 2: 1x1 Depth32Float, cleared to far (1.0) once and reused as the
 * shadow-map fragment binding whenever s_shadow_depth isn't ready. It must
 * NEVER be s_scene_depth: that texture is the scene encoder's live depth
 * ATTACHMENT for the very draw doing the sampling, and binding a render
 * target as a shader resource on the same encoder is a read-while-write
 * hazard (undefined per the Metal spec, flagged by the validation layer). */
static id<MTLTexture> s_shadow_dummy_depth = nil;
static id<MTLTexture> s_snapshot_tex = nil;  /* XLU/RDP framebuffer snapshot (3.3) */
static id<MTLTexture> s_final_color = nil;   /* Phase 4 output-filter result (present + readback source) */
static id<MTLTexture> s_filter_low = nil;    /* Phase 4 output-filter 8-bit intermediate (GL's pre-pass low_tex) */
/* SSAO v2 (W3.E2) — hemisphere AO pass chain (Metal only). s_ssao_raw holds the
 * PASS A result (and the PASS B ping-pong destination); s_ssao_tmp is the blur
 * intermediate. R8Unorm, RenderTarget|ShaderRead, sized w>>hr,h>>hr. */
static id<MTLTexture> s_ssao_raw = nil;
static id<MTLTexture> s_ssao_tmp = nil;
static int s_ssao_w = 0, s_ssao_h = 0, s_ssao_hr = -1;  /* AO target size + half-res state */
/* SMAA (W3.E4) — 3-pass morphological AA (Metal only). Scene-sized targets:
 * edges RG8, blend RGBA8, out BGRA8 (BGRA8 matches s_scene_color so the output
 * filter's source swap is a transparent one-array-entry change). Lazily allocated
 * like the SSAO targets so a SMAA alloc failure disables SMAA without touching the
 * scene path. The two committed LUTs (AreaTex/SearchTex) upload once via
 * replaceRegion. All private except the LUTs (shared, replaceRegion-uploaded). */
static id<MTLTexture> s_smaa_edges = nil;
static id<MTLTexture> s_smaa_blend = nil;
static id<MTLTexture> s_smaa_out = nil;
static int s_smaa_w = 0, s_smaa_h = 0;
static id<MTLSamplerState> s_snapshot_sampler = nil;
static id<MTLSamplerState> s_depth_sampler = nil;  /* nearest/clamp for SSAO depth reads (Phase 5) */
static int s_fb_w = 0, s_fb_h = 0;
static id<MTLRenderCommandEncoder> s_enc = nil;
static id<MTLTexture> s_white_tex = nil;   /* 1x1 white for unbound tiles */
/* The texture actually presented this frame — what readback/screenshots must
 * read (post-filter when the filter ran, else the raw scene). GL reads its
 * post-filter default framebuffer; this keeps Metal screenshots consistent. */
static id<MTLTexture> s_readback_src = nil;
static id<MTLRenderPipelineState> s_minimap_overlay_pso = nil;
static id<MTLTexture> s_minimap_overlay_target = nil;

/* Deferred render state (set by the vtable setters, resolved in draw_triangles). */
static enum GfxBlendMode s_blend = GFX_BLEND_DISABLED;
static bool s_preserve_cov_alpha = false;
static bool s_depth_test = false, s_depth_update = false, s_depth_compare = false;
static uint16_t s_zmode = 0;
static uint32_t s_tile_tex[2] = {0, 0};
static bool s_tile_linear[2] = {false, false};
static uint32_t s_tile_cms[2] = {0, 0}, s_tile_cmt[2] = {0, 0};
static uint32_t s_last_selected_id = 0;
static int s_vp_x = 0, s_vp_y = 0, s_vp_w = 0, s_vp_h = 0;
static int s_sc_x = 0, s_sc_y = 0, s_sc_w = 0, s_sc_h = 0;
static bool s_sc_set = false;
static uint32_t s_frame_count_metal = 0;

/* --- W3.E1: GPU timers + programmatic capture (diagnostic, OFF by default) ---
 * Everything here is gated on GE007_METAL_GPU_TRACE / GE007_METAL_CAPTURE; when
 * those envs are unset NO counter buffer is created, NO extra encoder/attachment
 * is added, and NO signpost fires — so a default frame is byte-identical to the
 * pre-W3.E1 backend (R3). total_ms comes from the command buffer's GPU
 * start/end (seconds domain); the scene/post split is a stage-boundary
 * MTLCounterSampleBuffer measuring the post passes directly, with scene derived
 * as (total - post). Falls back to total-only when the device/counter set does
 * not support stage-boundary timestamp sampling (§4.8). */
#define MTL_GPU_TS_PER_FRAME 12   /* max sampled post passes*2 per in-flight frame */
#define MTL_GPU_TS_SLOTS     (MTL_VBUF_SLOTS * MTL_GPU_TS_PER_FRAME)
static int    s_gpu_trace = -1;              /* -1 unread; 0/1 GE007_METAL_GPU_TRACE */
static bool   s_gpu_ts_checked = false;      /* one-shot support probe done */
static bool   s_gpu_ts_ready = false;        /* sample buffer usable */
static id<MTLCounterSampleBuffer> s_gpu_ts_buf = nil;
static int    s_frame_ts_slot = -1;          /* reserved slot: whole-frame start ts */
static int    s_frame_ts_written = 0;        /* 1 once the scene pass attached it */
static int    s_post_ts_next = 0;            /* next free slot this frame */
static int    s_post_ts_limit = 0;           /* one past this frame's slot window */
static int    s_post_ts_first = -1;          /* first (post-begin) slot this frame */
static int    s_post_ts_last = -1;           /* last (post-end) slot this frame */
static os_signpost_id_t s_sp_scene = 0;      /* scene interval id for this frame */
static bool mtl_gpu_trace_on(void);          /* defined below; used in mtl_open_scene_encoder */
static void mtl_gpu_ts_attach_frame_start(MTLRenderPassDescriptor *rpd);

/* Programmatic GPU capture (Xcode .gputrace). GE007_METAL_CAPTURE=<frame>.
 * -2 unread; -1 disabled/done; >=0 = target frame (s_frame_count_metal). */
static long s_gpu_capture_frame = -2;
static bool s_gpu_capturing = false;         /* a capture spans the current frame */

/* Triple-buffered vertex arena — replaces a per-draw newBufferWithBytes (which
 * was thousands of driver allocations/frame on foliage/glass scenes). Each real
 * frame bump-allocates from one of MTL_VBUF_SLOTS StorageModeShared buffers; a
 * dispatch_semaphore throttles the CPU to <= MTL_VBUF_SLOTS frames ahead so a
 * slot is only reused after its GPU work completes. Draws that overflow a slot
 * fall back to a one-off buffer (rare; grows the high-water for next (re)alloc). */
#define MTL_VBUF_SLOTS 3
static id<MTLBuffer> s_vbuf[MTL_VBUF_SLOTS];
static size_t s_vbuf_cap[MTL_VBUF_SLOTS];
/* Shadow-caster vertex buffer ring (§3.4 fix) — indexed by s_vbuf_slot,
 * exactly like s_vbuf above, so the CPU never memcpy's into a slot the GPU
 * may still be reading from an in-flight shadow pass. */
static id<MTLBuffer> s_shadow_vbuf[MTL_VBUF_SLOTS];
static NSUInteger s_shadow_vbuf_cap[MTL_VBUF_SLOTS];
static int s_vbuf_slot = 0;
static size_t s_vbuf_cursor = 0;
static size_t s_vbuf_want = (2u << 20);  /* high-water target for slot (re)alloc; 2 MB seed */
static int s_ring_index = 0;             /* advances per REAL frame (cmdbuf created), not per skip */
static dispatch_semaphore_t s_frame_sem = nil;

/* Texture registry (id -> MTLTexture) + pipeline/depth/sampler caches. */
static NSMutableDictionary<NSNumber *, id<MTLTexture>> *s_textures = nil;
static NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> *s_depth_cache = nil;
static NSMutableDictionary<NSNumber *, id<MTLSamplerState>> *s_sampler_cache = nil;

static id<MTLTexture> mtl_lookup_tex(uint32_t id) {
    if (id == 0 || s_textures == nil) return nil;
    return s_textures[@(id)];
}

/* Video.VSync for the Metal backend. platformApplyVSync's SDL_GL_SetSwapInterval
 * is a GL-only no-op here; the CAMetalLayer equivalent is displaySyncEnabled
 * (default YES). Stored in a static because the platform applies vsync before
 * mtl_init has acquired the layer; mtl_init re-applies the stored value. */
static bool s_vsync_enabled = true;

extern "C" void gfx_metal_set_vsync(int enabled) {
    s_vsync_enabled = (enabled != 0);
    if (s_layer != nil) {
        s_layer.displaySyncEnabled = s_vsync_enabled ? YES : NO;
    }
}

/* Port of gfx_opengl_axis_filter_scale (gfx_opengl.c:594-615). */
static float mtl_axis_filter_scale(uint32_t drawable_size, int logical_size, int fallback) {
    if (logical_size <= 0) logical_size = fallback;
    if (drawable_size == 0 || logical_size <= 0) return 1.0f;
    float scale = (float)drawable_size / (float)logical_size;
    if (scale < 1.0f) return 1.0f;
    if (scale > 64.0f) return 64.0f;
    return scale;
}

static MTLSamplerAddressMode mtl_wrap(uint32_t v) {
    if (v & GE007_G_TX_CLAMP) return MTLSamplerAddressModeClampToEdge;
    return (v & GE007_G_TX_MIRROR) ? MTLSamplerAddressModeMirrorRepeat
                                   : MTLSamplerAddressModeRepeat;
}

extern "C" int gfx_metal_max_offscreen_dim(void);  /* defined below */

/* FID-0018: alpha-to-coverage master switch, mirroring the GL negative control
 * (gfx_opengl.c gfx_opengl_a2c_enabled / GE007_NO_A2C=1). A2C only ever engages
 * when MSAA is active AND the material is an alpha-tested cutout, so this gates
 * that; it is a no-op when MSAA is off. */
static bool mtl_a2c_enabled(void) {
    static int force = -1;
    if (force < 0) {
        const char *e = getenv("GE007_NO_A2C");
        force = (e && e[0] && strcmp(e, "0") != 0) ? 0 : 1;
    }
    return force != 0;
}

/* FID-0018 negative control: GE007_NO_METAL_MSAA=1 forces the Metal backend back
 * to its pre-fix single-sample behavior regardless of Video.MSAA (the A/B opt-out
 * for the whole fix — with it set, edges alias exactly as before; unset, the
 * setting is honored). Byte-identical to pre-fix when set. */
static bool mtl_msaa_force_off(void) {
    static int force = -1;
    if (force < 0) {
        const char *e = getenv("GE007_NO_METAL_MSAA");
        force = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return force != 0;
}

/* FID-0019 negative control (P1b): GE007_NO_METAL_SHADOW_DEPTH_CLAMP=1 reverts
 * mtl_shadow_encode to its pre-fix (dc55008) behavior — the depth-only shadow
 * pass leaves the fresh encoder in Metal's default MTLDepthClipModeClip, so a
 * boundary caster whose light-space z runs past the shadow ortho's [0,4*radius]
 * range is near/far-CLIPPED out of the shadow map here, where GL's process-global
 * GL_DEPTH_CLAMP (gfx_opengl.c:3734) clamps it instead. Off (default) = fix active
 * (clamp), matching GL. Metal-only: the default GL backend is unaffected either
 * way, so tools/sim_invariance_gate.sh and the GL faithful path stay byte-identical
 * (charter rule 5). A/B: metal_shadow_clamp_regression.sh. */
static bool mtl_shadow_depth_clamp_disabled(void) {
    static int force = -1;
    if (force < 0) {
        force = port_env_bool("GE007_NO_METAL_SHADOW_DEPTH_CLAMP", 0,
                              "Revert the Metal sun-shadow depth clamp to GL-parity "
                              "off (fix active by default; Metal-only)");
    }
    return force != 0;
}

/* FID-0019 negative control (P1b): GE007_NO_METAL_SHADOW_DUMMY_DEPTH=1 reverts the
 * sun-shadow receiver fallback (mtl_draw_triangles) to its pre-fix (73bbdac)
 * behavior — binding the live scene depth attachment (s_scene_depth) as the
 * shadow-map fragment sampler when s_shadow_depth is nil. That is a read-while-
 * write hazard: s_scene_depth is THIS encoder's live depth ATTACHMENT for the
 * same draw. Off (default) = fix active (cleared 1x1 dummy = "fully lit", matching
 * GL's gfx_opengl.c:1310 out-of-frustum rule). This fallback is unreachable in
 * normal play (SHADER_OPT_SUN_SHADOW is only set when g_pc_shadow_map_ready=1,
 * which requires s_shadow_depth non-nil — see mtl_render_shadow_map), so the flag
 * only changes behavior under a forced invariant break. Metal-only. */
static bool mtl_shadow_dummy_depth_disabled(void) {
    static int force = -1;
    if (force < 0) {
        force = port_env_bool("GE007_NO_METAL_SHADOW_DUMMY_DEPTH", 0,
                              "Revert the Metal sun-shadow receiver dummy-depth fallback "
                              "(fix active by default; Metal-only)");
    }
    return force != 0;
}

/* Effective, device-clamped scene-pass sample count for Video.MSAA (1 = off).
 * Reads the live setting each call (Video.MSAA is SETTING_SCOPE_LIVE) so a
 * runtime toggle re-sizes the targets next frame. The device-support mask is
 * probed once. Logs a one-shot line whenever the resolved count changes, so the
 * "setting took effect" evidence is visible in the backend's own output. */
static int mtl_effective_msaa_samples(void) {
    if (mtl_msaa_force_off() || s_device == nil) return 1;

    static unsigned sup_mask = 0;
    static bool probed = false;
    if (!probed) {
        probed = true;
        if ([s_device supportsTextureSampleCount:2]) sup_mask |= GFX_MSAA_SUP_2;
        if ([s_device supportsTextureSampleCount:4]) sup_mask |= GFX_MSAA_SUP_4;
        if ([s_device supportsTextureSampleCount:8]) sup_mask |= GFX_MSAA_SUP_8;
    }

    int requested = g_pcMsaaSamples;
    int effective = gfxMsaaResolveSampleCount(requested, sup_mask);

    static int last_req = -0x7fffffff, last_eff = -0x7fffffff;
    if (requested != last_req || effective != last_eff) {
        if (requested >= 2 && effective != requested) {
            fprintf(stderr, "[metal] Video.MSAA=%d clamped to %dx (device support mask 0x%x)\n",
                    requested, effective, sup_mask);
        } else {
            fprintf(stderr, "[metal] MSAA: Video.MSAA=%d -> rasterSampleCount %d%s\n",
                    requested, effective, effective > 1 ? "x" : " (off)");
        }
        fflush(stderr);
        last_req = requested;
        last_eff = effective;
    }
    return effective;
}

/* True while the scene pass is rendering multisampled (this frame's resolved
 * count > 1). Used to disable SSAO under MSAA (GL contract) and to gate A2C. */
static inline bool mtl_msaa_active(void) { return s_msaa_samples > 1; }

static void mtl_ensure_targets(int w, int h) {
    if (w <= 0 || h <= 0) return;
    int cap = gfx_metal_max_offscreen_dim();
    if (w > cap) w = cap;
    if (h > cap) h = cap;
    /* FID-0018: re-resolve the effective sample count (Video.MSAA is live) and
     * re-allocate when it changes, so a runtime MSAA toggle takes effect. When
     * off (want_samples == 1) the MS attachments are freed and the pass reverts
     * to the single-sample path = byte-identical. */
    int want_samples = mtl_effective_msaa_samples();
    /* Don't re-attempt an MS allocation that already failed at this exact size
     * (VRAM pressure) — that would thrash every target + respam the warning each
     * frame. Fall back to single-sample until the request or the size changes. */
    if (want_samples > 1 && want_samples == s_msaa_failed_req &&
        s_fb_w == w && s_fb_h == h) {
        want_samples = 1;
    }
    if (s_scene_color != nil && s_fb_w == w && s_fb_h == h &&
        s_msaa_samples == want_samples) return;
    /* Scene color: render target + blit source (present/readback/snapshot) AND
     * the Phase-4 output-filter source (sampled/read in the filter fragment), so
     * it needs ShaderRead. */
    MTLTextureDescriptor *cd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:w height:h mipmapped:NO];
    cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    cd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> color = [s_device newTextureWithDescriptor:cd];
    /* Depth: render target + ShaderRead so Phase 5 SSAO can sample it (the
     * render pass stores it — storeAction=Store in mtl_open_scene_encoder). This
     * native Depth32Float sample is the whole point: it op-hangs GL-over-Metal. */
    MTLTextureDescriptor *dd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                           width:w height:h mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    dd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [s_device newTextureWithDescriptor:dd];
    /* XLU/RDP snapshot target: a copy of the scene color the fragment samples
     * mid-frame (Phase 3.3). Same format so the blit-copy is exact. */
    MTLTextureDescriptor *sd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:w height:h mipmapped:NO];
    sd.usage = MTLTextureUsageShaderRead;
    sd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> snap = [s_device newTextureWithDescriptor:sd];
    /* Phase-4 output-filter result: render target + blit source (present) +
     * readback source. */
    MTLTextureDescriptor *fd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:w height:h mipmapped:NO];
    fd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    fd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> finalc = [s_device newTextureWithDescriptor:fd];
    id<MTLTexture> low = [s_device newTextureWithDescriptor:fd];  /* same desc: RT + ShaderRead */

    if (color == nil || depth == nil || snap == nil || finalc == nil || low == nil) {
        /* Partial allocation failure: bind nothing broken. Leave s_fb_w/h
         * unchanged so the line-above early-out doesn't latch a bad size and
         * start_frame's `s_scene_color == nil` guard skips the frame; next frame
         * retries. */
        fprintf(stderr, "[metal] WARNING: scene target allocation failed (%dx%d)\n", w, h);
        s_scene_color = nil;
        s_scene_depth = nil;
        s_scene_color_ms = nil;
        s_scene_depth_ms = nil;
        s_msaa_samples = 1;
        s_snapshot_tex = nil;
        s_final_color = nil;
        s_filter_low = nil;
        s_readback_src = nil;
        return;
    }
    /* FID-0018: multisample scene attachments. Only the color needs a resolve
     * target (StoreAndMultisampleResolve -> s_scene_color); neither is ever
     * sampled, so ShaderRead is not requested. Allocation failure downgrades to
     * single-sample (want_samples := 1) rather than dropping the frame — the
     * scene still renders, just without MSAA. */
    id<MTLTexture> color_ms = nil, depth_ms = nil;
    if (want_samples > 1) {
        MTLTextureDescriptor *cmd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:w height:h mipmapped:NO];
        cmd.textureType = MTLTextureType2DMultisample;
        cmd.sampleCount = (NSUInteger)want_samples;
        cmd.usage = MTLTextureUsageRenderTarget;
        cmd.storageMode = MTLStorageModePrivate;
        color_ms = [s_device newTextureWithDescriptor:cmd];

        MTLTextureDescriptor *dmd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:w height:h mipmapped:NO];
        dmd.textureType = MTLTextureType2DMultisample;
        dmd.sampleCount = (NSUInteger)want_samples;
        dmd.usage = MTLTextureUsageRenderTarget;
        dmd.storageMode = MTLStorageModePrivate;
        depth_ms = [s_device newTextureWithDescriptor:dmd];

        if (color_ms == nil || depth_ms == nil) {
            fprintf(stderr, "[metal] WARNING: %dx MSAA target alloc failed (%dx%d) — MSAA off this frame\n",
                    want_samples, w, h);
            color_ms = nil;
            depth_ms = nil;
            s_msaa_failed_req = want_samples;  /* latch: don't retry this combo */
            want_samples = 1;
        } else {
            s_msaa_failed_req = 0;  /* succeeded — clear any prior failure latch */
        }
    }

    s_scene_color = color;
    s_scene_depth = depth;
    s_scene_color_ms = color_ms;   /* nil when MSAA off (frees the prior MS texture via ARC) */
    s_scene_depth_ms = depth_ms;
    s_msaa_samples = want_samples;
    s_snapshot_tex = snap;
    s_final_color = finalc;
    s_filter_low = low;
    s_readback_src = color;  /* until a frame sets it */
    s_fb_w = w;
    s_fb_h = h;
    /* Match the drawable to the render resolution so the end_frame blit is
     * size-exact; the compositor scales the drawable to the window (this is the
     * SSAA downsample, mirroring GL's resolve_scene_target blit). */
    if (s_layer != nil) s_layer.drawableSize = CGSizeMake(w, h);
}

static void mtl_ensure_white(void) {
    if (s_white_tex != nil) return;
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1 height:1 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    s_white_tex = [s_device newTextureWithDescriptor:d];
    if (s_white_tex == nil) return;  /* draw falls back to whatever is bound; no crash */
    uint8_t white[4] = {255, 255, 255, 255};
    [s_white_tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:white bytesPerRow:4];
}

static void mtl_build_vertex_descriptor(MetalShader *ms) {
    if (ms->vtxDesc != nil) return;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (int i = 0; i < ms->numAttrs; i++) {
        MtlAttr a = ms->attrs[i];
        MTLVertexFormat fmt = a.size == 1 ? MTLVertexFormatFloat
                            : a.size == 2 ? MTLVertexFormatFloat2
                            : a.size == 3 ? MTLVertexFormatFloat3
                                          : MTLVertexFormatFloat4;
        vd.attributes[a.index].format = fmt;
        vd.attributes[a.index].offset = (NSUInteger)a.offset * sizeof(float);
        vd.attributes[a.index].bufferIndex = 0;
    }
    vd.layouts[0].stride = (NSUInteger)ms->numFloats * sizeof(float);
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    vd.layouts[0].stepRate = 1;
    ms->vtxDesc = vd;
}

/* Metal bakes blend/format/samples/writeMask into the PSO, so the GL immediate
 * setters collapse to this lazily-cached lookup keyed on the dynamic state. */
static id<MTLRenderPipelineState> mtl_pso_for(MetalShader *ms, enum GfxBlendMode blend,
                                              int samples, bool alpha_to_coverage,
                                              bool write_alpha) {
    uint64_t key = (uint64_t)(blend & 0xF) |
                   ((uint64_t)(samples & 0xFF) << 4) |
                   ((uint64_t)(write_alpha ? 1 : 0) << 12) |
                   ((uint64_t)(alpha_to_coverage ? 1 : 0) << 13);
    /* Fast path: consecutive draws usually share shader+state — skip the
     * NSNumber boxing + dictionary lookup (and its autoreleased temporary). */
    static MetalShader *last_ms = nullptr;
    static uint64_t last_key = ~0ull;
    static id<MTLRenderPipelineState> last_pso = nil;
    if (ms == last_ms && key == last_key && last_pso != nil) return last_pso;
    NSNumber *k = @(key);
    id<MTLRenderPipelineState> pso = ms->psoCache[k];
    if (pso != nil) { last_ms = ms; last_key = key; last_pso = pso; return pso; }

    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = ms->vtxFn;
    d.fragmentFunction = ms->fragFn;
    d.vertexDescriptor = ms->vtxDesc;
    d.rasterSampleCount = samples;
    /* FID-0018: alpha-to-coverage for alpha-tested cutout materials under MSAA
     * (mirrors GL's GL_SAMPLE_ALPHA_TO_COVERAGE — gfx_opengl_update_a2c_state).
     * The caller only sets this when samples > 1 and the material qualifies, so
     * it is a no-op on the single-sample path (Metal ignores it at 1 sample). */
    d.alphaToCoverageEnabled = alpha_to_coverage;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

    MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
    MTLColorWriteMask wm = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
    if (write_alpha) wm |= MTLColorWriteMaskAlpha;
    ca.writeMask = wm;

    /* Blend factors mirror gfx_opengl_set_blend_mode:1619-1647 (default diag). */
    bool enable = (blend == GFX_BLEND_ALPHA || blend == GFX_BLEND_MODULATE ||
                   blend == GFX_BLEND_ALPHA_COVERAGE ||
                   blend == GFX_BLEND_ALPHA_CVG_WRAP_STENCIL);
    ca.blendingEnabled = enable;
    if (enable) {
        if (blend == GFX_BLEND_MODULATE) {
            ca.sourceRGBBlendFactor = MTLBlendFactorDestinationColor;
            ca.destinationRGBBlendFactor = MTLBlendFactorZero;
            ca.sourceAlphaBlendFactor = MTLBlendFactorDestinationAlpha;
            ca.destinationAlphaBlendFactor = MTLBlendFactorZero;
        } else {
            ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            ca.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
            ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        ca.rgbBlendOperation = MTLBlendOperationAdd;
        ca.alphaBlendOperation = MTLBlendOperationAdd;
    }

    NSError *err = nil;
    pso = [s_device newRenderPipelineStateWithDescriptor:d error:&err];
    if (pso == nil) {
        fprintf(stderr, "[metal] FATAL: PSO build failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
        abort();
    }
    ms->psoCache[k] = pso;
    last_ms = ms; last_key = key; last_pso = pso;
    return pso;
}

static id<MTLDepthStencilState> mtl_depth_state_for(bool test, bool update, bool compare, uint16_t zmode) {
    uint64_t key = (test ? 1 : 0) | (update ? 2 : 0) | (compare ? 4 : 0) | ((uint64_t)zmode << 3);
    static uint64_t last_key = ~0ull;
    static id<MTLDepthStencilState> last_ds = nil;
    if (key == last_key && last_ds != nil) return last_ds;
    NSNumber *k = @(key);
    id<MTLDepthStencilState> ds = s_depth_cache[k];
    if (ds != nil) { last_key = key; last_ds = ds; return ds; }
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
    /* GL: test+compare -> LEQUAL (all live zmodes); test+!compare -> ALWAYS
     * (write-only); !test -> disabled (ALWAYS, no write). */
    dd.depthCompareFunction = (test && compare) ? MTLCompareFunctionLessEqual
                                                : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = (test && update);
    ds = [s_device newDepthStencilStateWithDescriptor:dd];
    s_depth_cache[k] = ds;
    last_key = key; last_ds = ds;
    return ds;
}

static bool mtl_ensure_minimap_overlay_pso(void) {
    if (s_minimap_overlay_pso != nil) return true;
    if (s_device == nil) return false;

  @autoreleasepool {  /* MSL compile transients (M3.2) */
    static const char *src =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct OverlayVertex {\n"
        "  float2 position [[attribute(0)]];\n"
        "  float4 color [[attribute(1)]];\n"
        "};\n"
        "struct OverlayUniforms { float2 screen; };\n"
        "struct OverlayOut {\n"
        "  float4 position [[position]];\n"
        "  float4 color;\n"
        "};\n"
        "vertex OverlayOut minimapOverlayVertex(OverlayVertex in [[stage_in]],\n"
        "                                      constant OverlayUniforms& u [[buffer(1)]]) {\n"
        "  float2 ndc = float2((in.position.x / u.screen.x) * 2.0 - 1.0,\n"
        "                       1.0 - (in.position.y / u.screen.y) * 2.0);\n"
        "  OverlayOut out;\n"
        "  out.position = float4(ndc, 0.0, 1.0);\n"
        "  out.color = in.color;\n"
        "  return out;\n"
        "}\n"
        "fragment float4 minimapOverlayFragment(OverlayOut in [[stage_in]]) {\n"
        "  return in.color;\n"
        "}\n";

    NSError *err = nil;
    NSString *nssrc = [NSString stringWithUTF8String:src];
    id<MTLLibrary> lib = [s_device newLibraryWithSource:nssrc options:nil error:&err];
    if (lib == nil) {
        fprintf(stderr,
                "[metal] minimap overlay MSL compile failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
        return false;
    }

    id<MTLFunction> vfn = [lib newFunctionWithName:@"minimapOverlayVertex"];
    id<MTLFunction> ffn = [lib newFunctionWithName:@"minimapOverlayFragment"];
    if (vfn == nil || ffn == nil) {
        fprintf(stderr, "[metal] minimap overlay MSL missing entry point\n");
        return false;
    }

    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2;
    vd.attributes[0].offset = 0;
    vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat4;
    vd.attributes[1].offset = 2 * sizeof(float);
    vd.attributes[1].bufferIndex = 0;
    vd.layouts[0].stride = 6 * sizeof(float);
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    vd.layouts[0].stepRate = 1;

    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vfn;
    pd.fragmentFunction = ffn;
    pd.vertexDescriptor = vd;
    pd.rasterSampleCount = 1;
    MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ca.blendingEnabled = YES;
    ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    ca.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    ca.rgbBlendOperation = MTLBlendOperationAdd;
    ca.alphaBlendOperation = MTLBlendOperationAdd;

    s_minimap_overlay_pso = [s_device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (s_minimap_overlay_pso == nil) {
        fprintf(stderr,
                "[metal] minimap overlay PSO build failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
        return false;
    }

    return true;
  }
}

extern "C" int gfx_metal_draw_minimap_overlay(const void *vertices,
                                              size_t vertex_count,
                                              int fb_width,
                                              int fb_height,
                                              int scissor_enabled,
                                              int scissor_x,
                                              int scissor_y,
                                              int scissor_w,
                                              int scissor_h) {
    @autoreleasepool {
        if (vertices == NULL || vertex_count == 0) return 1;
        if (s_cmdbuf == nil || s_minimap_overlay_target == nil) return 0;
        if (!mtl_ensure_minimap_overlay_pso()) return 0;

        const size_t vertex_stride = 6 * sizeof(float);
        if (vertex_count > ((size_t)-1) / vertex_stride) return 0;
        int target_w = (int)s_minimap_overlay_target.width;
        int target_h = (int)s_minimap_overlay_target.height;
        if (target_w <= 0 || target_h <= 0) return 0;
        if (fb_width <= 0) fb_width = target_w;
        if (fb_height <= 0) fb_height = target_h;

        int sx = 0;
        int sy = 0;
        int sw = target_w;
        int sh = target_h;
        if (scissor_enabled) {
            sx = scissor_x;
            sy = fb_height - (scissor_y + scissor_h);
            sw = scissor_w;
            sh = scissor_h;
            if (sx < 0) { sw += sx; sx = 0; }
            if (sy < 0) { sh += sy; sy = 0; }
            if (sx > target_w) sx = target_w;
            if (sy > target_h) sy = target_h;
            if (sx + sw > target_w) sw = target_w - sx;
            if (sy + sh > target_h) sh = target_h - sy;
            if (sw <= 0 || sh <= 0) return 1;
        }

        size_t need = vertex_count * vertex_stride;
        id<MTLBuffer> vbuf;
        NSUInteger voff;
        size_t aligned = (s_vbuf_cursor + 255u) & ~(size_t)255u;
        if (s_vbuf[s_vbuf_slot] != nil && aligned + need <= s_vbuf_cap[s_vbuf_slot]) {
            vbuf = s_vbuf[s_vbuf_slot];
            voff = (NSUInteger)aligned;
            memcpy((uint8_t *)vbuf.contents + aligned, vertices, need);
            s_vbuf_cursor = aligned + need;
            if (s_vbuf_cursor > s_vbuf_want) s_vbuf_want = s_vbuf_cursor;
        } else {
            if (aligned + need > s_vbuf_want) s_vbuf_want = aligned + need;
            vbuf = [s_device newBufferWithBytes:vertices
                                         length:need
                                        options:MTLResourceStorageModeShared];
            voff = 0;
        }
        if (vbuf == nil) return 0;

        MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = s_minimap_overlay_target;
        rpd.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [s_cmdbuf renderCommandEncoderWithDescriptor:rpd];
        if (enc == nil) return 0;

        struct {
            float screen[2];
        } uniforms = {{ (float)fb_width, (float)fb_height }};

        [enc setRenderPipelineState:s_minimap_overlay_pso];
        [enc setViewport:(MTLViewport){0.0, 0.0, (double)target_w, (double)target_h, 0.0, 1.0}];
        [enc setScissorRect:(MTLScissorRect){(NSUInteger)sx,
                                             (NSUInteger)sy,
                                             (NSUInteger)sw,
                                             (NSUInteger)sh}];
        [enc setVertexBuffer:vbuf offset:voff atIndex:0];
        [enc setVertexBytes:&uniforms length:sizeof uniforms atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)vertex_count];
        [enc endEncoding];
        return 1;
    }
}

static id<MTLSamplerState> mtl_sampler_for(bool linear, uint32_t cms, uint32_t cmt) {
    uint64_t key = (linear ? 1 : 0) | ((uint64_t)(cms & 0xFFFF) << 1) | ((uint64_t)(cmt & 0xFFFF) << 17);
    static uint64_t last_key = ~0ull;
    static id<MTLSamplerState> last_s = nil;
    if (key == last_key && last_s != nil) return last_s;
    NSNumber *k = @(key);
    id<MTLSamplerState> s = s_sampler_cache[k];
    if (s != nil) { last_key = key; last_s = s; return s; }
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    MTLSamplerMinMagFilter f = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.minFilter = f;
    sd.magFilter = f;
    sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
    sd.sAddressMode = mtl_wrap(cms);
    sd.tAddressMode = mtl_wrap(cmt);
    sd.maxAnisotropy = linear ? 16 : 1;
    s = [s_device newSamplerStateWithDescriptor:sd];
    s_sampler_cache[k] = s;
    last_key = key; last_s = s;
    return s;
}

/* Open the scene render encoder. clear=true clears color+depth (frame start);
 * clear=false preserves prior contents (Load) — used to resume after an XLU/RDP
 * snapshot forces an encoder break. */
static void mtl_open_scene_encoder(bool clear) {
    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    /* FID-0018: when MSAA is active the pass renders into the multisample color
     * attachment and resolves into the single-sample s_scene_color at every
     * endEncoding (StoreAndMultisampleResolve), so the mid-frame snapshot copy,
     * the output filter, the present blit and readback all read the resolved
     * scene exactly as in the single-sample path. StoreAndMultisampleResolve also
     * keeps the MS samples, so a mid-frame snapshot reopen (loadAction=Load)
     * restores them. When MSAA is off both branches bind s_scene_color/-depth
     * with storeAction=Store = byte-identical to pre-fix. */
    if (mtl_msaa_active() && s_scene_color_ms != nil && s_scene_depth_ms != nil) {
        rpd.colorAttachments[0].texture = s_scene_color_ms;
        rpd.colorAttachments[0].resolveTexture = s_scene_color;
        rpd.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
        if (clear) rpd.colorAttachments[0].clearColor = MTLClearColorMake(s_clear_r, s_clear_g, s_clear_b, 1.0);
        /* MS depth is kept (Store) so a snapshot reopen can Load it; it is never
         * sampled (SSAO is disabled under MSAA, mirroring the GL contract). */
        rpd.depthAttachment.texture = s_scene_depth_ms;
        rpd.depthAttachment.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        rpd.depthAttachment.storeAction = MTLStoreActionStore;
        if (clear) rpd.depthAttachment.clearDepth = 1.0;
    } else {
    rpd.colorAttachments[0].texture = s_scene_color;
    rpd.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (clear) rpd.colorAttachments[0].clearColor = MTLClearColorMake(s_clear_r, s_clear_g, s_clear_b, 1.0);
    rpd.depthAttachment.texture = s_scene_depth;
    rpd.depthAttachment.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    rpd.depthAttachment.storeAction = MTLStoreActionStore;
    if (clear) rpd.depthAttachment.clearDepth = 1.0;
    }
    /* W3.E1: timestamp the whole GPU frame's START on the first (clearing) scene
     * pass, so the post span can be self-calibrated against total_ms. */
    if (clear && mtl_gpu_trace_on()) mtl_gpu_ts_attach_frame_start(rpd);
    s_enc = [s_cmdbuf renderCommandEncoderWithDescriptor:rpd];
    [s_enc setDepthClipMode:MTLDepthClipModeClamp];
}

/* ---- init / frame lifecycle ---------------------------------------------- */

static bool mtl_init(void) {
    s_device = MTLCreateSystemDefaultDevice();
    if (s_device == nil) {
        /* No usable Metal device. The backend is already selected upstream and
         * there is no in-process GL fallback, so fail here with a clear cause
         * rather than aborting later inside newLibraryWithSource with a
         * misleading "MSL compile failed" message. */
        fprintf(stderr, "[metal] FATAL: MTLCreateSystemDefaultDevice returned nil — no usable Metal device\n");
        return false;
    }
    s_queue = [s_device newCommandQueue];
    if (s_queue == nil) {
        fprintf(stderr, "[metal] FATAL: failed to create command queue\n");
        return false;
    }
    s_frame_sem = dispatch_semaphore_create(MTL_VBUF_SLOTS);
    s_layer = (__bridge CAMetalLayer *)platformGetMetalLayer();
    if (s_layer != nil) {
        s_layer.device = s_device;
        s_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        /* NO: the scene is composited offscreen and blitted into the drawable at
         * end_frame; a framebufferOnly drawable can't be a blit destination. */
        s_layer.framebufferOnly = NO;
        /* Re-apply the vsync mode the platform recorded before the layer existed. */
        s_layer.displaySyncEnabled = s_vsync_enabled ? YES : NO;
    } else {
        fprintf(stderr, "[metal] FATAL: no CAMetalLayer from platform\n");
        return false;
    }
    s_textures = [NSMutableDictionary dictionary];
    s_depth_cache = [NSMutableDictionary dictionary];
    s_sampler_cache = [NSMutableDictionary dictionary];
    {
        /* Clamp+linear sampler for the XLU/RDP framebuffer snapshot (index 2). */
        MTLSamplerDescriptor *ss = [[MTLSamplerDescriptor alloc] init];
        ss.minFilter = MTLSamplerMinMagFilterLinear;
        ss.magFilter = MTLSamplerMinMagFilterLinear;
        ss.mipFilter = MTLSamplerMipFilterNotMipmapped;
        ss.sAddressMode = MTLSamplerAddressModeClampToEdge;
        ss.tAddressMode = MTLSamplerAddressModeClampToEdge;
        s_snapshot_sampler = [s_device newSamplerStateWithDescriptor:ss];
        /* SSAO depth sampler: NEAREST (no interpolation across depth
         * discontinuities) + clampToEdge; non-comparison (returns the depth). */
        MTLSamplerDescriptor *ds = [[MTLSamplerDescriptor alloc] init];
        ds.minFilter = MTLSamplerMinMagFilterNearest;
        ds.magFilter = MTLSamplerMinMagFilterNearest;
        ds.mipFilter = MTLSamplerMipFilterNotMipmapped;
        ds.sAddressMode = MTLSamplerAddressModeClampToEdge;
        ds.tAddressMode = MTLSamplerAddressModeClampToEdge;
        s_depth_sampler = [s_device newSamplerStateWithDescriptor:ds];
    }
    /* Invariance-critical (parent plan §2.1): the CPU clipper reads
     * g_depth_clamp_enabled; Metal uses native depth-clamp, so force it true. */
    g_depth_clamp_enabled = true;
    fprintf(stderr, "[metal] native Metal backend init: device='%s' layer=%p\n",
            s_device.name.UTF8String, (__bridge void *)s_layer);
    return true;
}

/* ---- W3.E1 GPU timers (§4.8) --------------------------------------------- */

static bool mtl_gpu_trace_on(void) {
    if (s_gpu_trace < 0) {
        const char *e = getenv("GE007_METAL_GPU_TRACE");
        s_gpu_trace = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s_gpu_trace == 1;
}

/* Points-of-interest log so Instruments' Metal System Trace labels the intervals
 * ("scene"/"post") we wrap the passes in. Lazily created; subsystem per §4.8. */
static os_log_t mtl_signpost_log(void) {
    static os_log_t log = NULL;
    if (log == NULL) log = os_log_create("com.mgb64.metal", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
    return log;
}

/* One-shot: probe stage-boundary timestamp support and allocate the sample
 * buffer. The tick->ms scale is derived per frame from the whole-frame span vs
 * total_ms (Apple counter timestamps are in a device-specific unit that does NOT
 * match the sampleTimestamps GPU domain, so a fixed scale is unreliable). On any
 * failure s_gpu_ts_ready stays false and timers report total-only (post=0). */
static void mtl_gpu_ts_ensure(void) {
    if (s_gpu_ts_checked) return;
    s_gpu_ts_checked = true;
    if (s_device == nil) return;
    if (![s_device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
        fprintf(stderr, "[PERF-GPU] stage-boundary timestamp sampling unsupported — total-only split\n");
        return;
    }
    id<MTLCounterSet> tsSet = nil;
    for (id<MTLCounterSet> cs in s_device.counterSets) {
        if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp]) { tsSet = cs; break; }
    }
    if (tsSet == nil) {
        fprintf(stderr, "[PERF-GPU] no timestamp counter set — total-only split\n");
        return;
    }
    MTLCounterSampleBufferDescriptor *d = [[MTLCounterSampleBufferDescriptor alloc] init];
    d.counterSet = tsSet;
    d.storageMode = MTLStorageModeShared;
    d.sampleCount = MTL_GPU_TS_SLOTS;
    NSError *err = nil;
    s_gpu_ts_buf = [s_device newCounterSampleBufferWithDescriptor:d error:&err];
    if (s_gpu_ts_buf == nil) {
        fprintf(stderr, "[PERF-GPU] counter sample buffer alloc failed: %s — total-only split\n",
                err ? err.localizedDescription.UTF8String : "(unknown)");
        return;
    }
    s_gpu_ts_ready = true;
}

/* Attach a whole-GPU-frame START timestamp (startOfVertex of the first scene
 * pass) to the given render pass, so post_ms can be self-calibrated against
 * total_ms. No-op unless tracing. Reserves this frame's slot base+0. */
static void mtl_gpu_ts_attach_frame_start(MTLRenderPassDescriptor *rpd) {
    if (!s_gpu_ts_ready || s_gpu_ts_buf == nil || s_frame_ts_slot < 0 || s_frame_ts_written) return;
    MTLRenderPassSampleBufferAttachmentDescriptor *a = rpd.sampleBufferAttachments[0];
    a.sampleBuffer = s_gpu_ts_buf;
    a.startOfVertexSampleIndex   = (NSUInteger)s_frame_ts_slot;
    a.endOfVertexSampleIndex     = MTLCounterDontSample;
    a.startOfFragmentSampleIndex = MTLCounterDontSample;
    a.endOfFragmentSampleIndex   = MTLCounterDontSample;
    s_frame_ts_written = 1;  /* consumed: attach to the first scene pass only */
}

/* Attach stage-boundary start/end timestamps to a POST render pass so the frame
 * span [first post-begin, last post-end] can be resolved after completion. No-op
 * unless tracing; silently skips once this frame's slot window is exhausted (the
 * measured span just ends at the last pass that fit). */
static void mtl_gpu_ts_attach_post(MTLRenderPassDescriptor *rpd) {
    if (!s_gpu_ts_ready || s_gpu_ts_buf == nil) return;
    if (s_post_ts_next + 2 > s_post_ts_limit) return;
    NSUInteger startIdx = (NSUInteger)s_post_ts_next++;
    NSUInteger endIdx   = (NSUInteger)s_post_ts_next++;
    MTLRenderPassSampleBufferAttachmentDescriptor *a = rpd.sampleBufferAttachments[0];
    a.sampleBuffer = s_gpu_ts_buf;
    a.startOfVertexSampleIndex   = startIdx;
    a.endOfVertexSampleIndex     = MTLCounterDontSample;
    a.startOfFragmentSampleIndex = MTLCounterDontSample;
    a.endOfFragmentSampleIndex   = endIdx;
    if (s_post_ts_first < 0) s_post_ts_first = (int)startIdx;
    s_post_ts_last = (int)endIdx;
}

/* ---- W3.E1 programmatic GPU capture (§4.8) ------------------------------- */

static long mtl_gpu_capture_target(void) {
    if (s_gpu_capture_frame == -2) {
        const char *e = getenv("GE007_METAL_CAPTURE");
        long v = (e && e[0]) ? strtol(e, NULL, 10) : -1;
        s_gpu_capture_frame = (v >= 0) ? v : -1;
    }
    return s_gpu_capture_frame;
}

/* Begin a programmatic capture of the current frame to ./mgb64_frame<N>.gputrace.
 * Outside Xcode the .gputrace destination is unavailable unless the process runs
 * with METAL_CAPTURE_ENABLED=1 — in that case log the reason (naming the env)
 * and disable; the run still exits normally. */
static void mtl_gpu_capture_begin(void) {
    MTLCaptureManager *mgr = [MTLCaptureManager sharedCaptureManager];
    if (![mgr supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
        fprintf(stderr,
                "[metal] GPU capture skipped for frame %u: .gputrace destination unavailable — "
                "set METAL_CAPTURE_ENABLED=1 in the environment, or capture interactively in "
                "Xcode (Debug > Capture GPU Workload)\n", s_frame_count_metal);
        s_gpu_capture_frame = -1;   /* one-shot: do not retry every frame */
        return;
    }
    NSString *path = [NSString stringWithFormat:@"mgb64_frame%u.gputrace", s_frame_count_metal];
    NSURL *url = [NSURL fileURLWithPath:path];
    /* startCaptureWithDescriptor fails if the .gputrace package already exists. */
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    MTLCaptureDescriptor *desc = [[MTLCaptureDescriptor alloc] init];
    desc.captureObject = s_queue;   /* this command queue's work for the frame */
    desc.destination = MTLCaptureDestinationGPUTraceDocument;
    desc.outputURL = url;
    NSError *err = nil;
    if (![mgr startCaptureWithDescriptor:desc error:&err]) {
        fprintf(stderr, "[metal] GPU capture start failed for frame %u: %s\n",
                s_frame_count_metal, err ? err.localizedDescription.UTF8String : "(unknown)");
        s_gpu_capture_frame = -1;
        return;
    }
    s_gpu_capturing = true;
    fprintf(stderr, "[metal] GPU capture STARTED for frame %u -> ./%s\n",
            s_frame_count_metal, path.UTF8String);
}

/* Stop the in-flight capture once the captured frame's command buffer completed,
 * flushing the .gputrace to disk. One-shot. */
static void mtl_gpu_capture_end(id<MTLCommandBuffer> cmdbuf) {
    if (!s_gpu_capturing) return;
    [cmdbuf waitUntilCompleted];   /* ensure the frame's GPU work is in the trace */
    [[MTLCaptureManager sharedCaptureManager] stopCapture];
    s_gpu_capturing = false;
    s_gpu_capture_frame = -1;       /* one-shot */
    fprintf(stderr, "[metal] GPU capture STOPPED for frame %u (./mgb64_frame%u.gputrace)\n",
            s_frame_count_metal, s_frame_count_metal);
}

/* W1.E3.T3: build the sun-shadow depth pipeline + target lazily (mirror of
 * gfx_opengl_ensure_shadow_resources). Depth-only PSO (nil color attachment,
 * Depth32Float), static shadowVertex/shadowFragment MSL. */
static void mtl_ensure_shadow_resources(int res) {
    if (res < 256) res = 2048;
    if (s_shadow_pso == nil) {
      @autoreleasepool {  /* MSL compile transients (M3.2) */
        static const char *src =
            "#include <metal_stdlib>\n"
            "using namespace metal;\n"
            "struct ShadowVin { float3 pos [[attribute(0)]]; };\n"
            "struct ShadowVout { float4 pos [[position]]; };\n"
            "vertex ShadowVout shadowVertex(ShadowVin in [[stage_in]],\n"
            "                               constant float4x4 &shadowMat [[buffer(1)]]) {\n"
            "    ShadowVout o; o.pos = shadowMat * float4(in.pos, 1.0);\n"
            "    /* gfx_compute_shadow_mat emits GL clip-z in [-1,1]; Metal NDC-z is\n"
            "     * [0,1], so apply the frontend z=(z+w)/2 remap here — makes the depth\n"
            "     * texture byte-match GL's (viewport maps GL clip-z the same way). */\n"
            "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"
            "    return o;\n"
            "}\n"
            "fragment void shadowFragment() {}\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [s_device newLibraryWithSource:[NSString stringWithUTF8String:src]
                                                    options:nil error:&err];
        if (lib == nil) {
            fprintf(stderr, "[metal] shadow MSL compile failed: %s\n",
                    err ? err.localizedDescription.UTF8String : "(no error)");
            return;
        }
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat3;
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.layouts[0].stride = 3 * sizeof(float);
        vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = [lib newFunctionWithName:@"shadowVertex"];
        pd.fragmentFunction = [lib newFunctionWithName:@"shadowFragment"];
        pd.vertexDescriptor = vd;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;  /* no color attachment */
        s_shadow_pso = [s_device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (s_shadow_pso == nil) {
            fprintf(stderr, "[metal] shadow PSO build failed: %s\n",
                    err ? err.localizedDescription.UTF8String : "(no error)");
            return;
        }
        MTLDepthStencilDescriptor *dsd = [[MTLDepthStencilDescriptor alloc] init];
        dsd.depthCompareFunction = MTLCompareFunctionLess;
        dsd.depthWriteEnabled = YES;
        s_shadow_ds = [s_device newDepthStencilStateWithDescriptor:dsd];
      }
    }
    if (s_shadow_cmp_sampler == nil) {
        /* T4 receiver: PCF comparison sampler — LessEqual + linear for hardware 2x2
         * PCF (mirrors GL's GL_COMPARE_REF_TO_TEXTURE/LEQUAL + GL_LINEAR). */
        MTLSamplerDescriptor *csd = [[MTLSamplerDescriptor alloc] init];
        csd.minFilter = MTLSamplerMinMagFilterLinear;
        csd.magFilter = MTLSamplerMinMagFilterLinear;
        csd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        csd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        csd.compareFunction = MTLCompareFunctionLessEqual;
        s_shadow_cmp_sampler = [s_device newSamplerStateWithDescriptor:csd];
    }
    if (s_shadow_depth == nil || s_shadow_res != res) {
        MTLTextureDescriptor *dd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:res height:res mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        dd.storageMode = MTLStorageModePrivate;
        s_shadow_depth = [s_device newTextureWithDescriptor:dd];
        s_shadow_res = (s_shadow_depth != nil) ? res : 0;
    }
}

/* M3.2 fix 2: lazily build the 1x1 dummy shadow-map fallback and clear it to
 * far (1.0) exactly once. GL's receiver treats "no valid shadow data" as fully
 * lit (gfx_opengl.c:1310, "outside the shadow frustum -> sh=1.0"); a depth of
 * 1.0 makes the receiver's `suv.z - bias <= storedDepth` compare pass for
 * every on-screen fragment, so every PCF tap (all sampling the same clamped
 * texel) reads lit, matching that rule instead of shadowing or reading
 * garbage. */
static void mtl_ensure_shadow_dummy(void) {
    if (s_shadow_dummy_depth != nil) return;
    MTLTextureDescriptor *dd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                           width:1 height:1 mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    dd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [s_device newTextureWithDescriptor:dd];
    if (tex == nil) return;
    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.depthAttachment.texture = tex;
    rpd.depthAttachment.loadAction = MTLLoadActionClear;
    rpd.depthAttachment.clearDepth = 1.0;
    rpd.depthAttachment.storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    s_shadow_dummy_depth = tex;
}

/* Encode the depth-only shadow draw into `enc` (front-face cull, slope bias),
 * matching gfx_opengl_render_shadow_map. Winding is CCW-front to mirror GL's
 * default so both backends cull the same faces (parity). */
static void mtl_shadow_encode(id<MTLRenderCommandEncoder> enc, id<MTLBuffer> vbuf,
                              size_t tri_count, const float *sm16) {
    const float *encode_mat = sm16;
    float test_mat[16];

    /* FID-0019 oracle fixture: normal authored scenes keep every captured caster
     * comfortably inside the light frustum, so Clamp and the legacy Clip mode
     * are byte-identical there. The Metal-only regression lane can translate
     * clip-space Z to put the REAL captured scene across the near plane and make
     * the API semantic observable. This is test instrumentation only (MGB64_,
     * deliberately outside the player-facing GE007_* flag surface); absent in
     * every normal launch and incapable of touching sim state. sm16 is the
     * column-major upload of g_pc_shadow_mat, so index 14 is row Z / column W. */
    const char *test_z = getenv("MGB64_TEST_METAL_SHADOW_CLIP_Z_OFFSET");
    if (test_z != NULL && test_z[0] != '\0') {
        char *end = NULL;
        float offset = strtof(test_z, &end);
        if (end != test_z && __builtin_isfinite(offset)) {
            memcpy(test_mat, sm16, sizeof(test_mat));
            test_mat[14] += offset;
            encode_mat = test_mat;
        }
    }

    [enc setRenderPipelineState:s_shadow_pso];
    [enc setDepthStencilState:s_shadow_ds];
    /* Metal's framebuffer y-axis is flipped vs GL, so a triangle GL sees as CCW is
     * CW here. Use CW-front so front-face culling removes the SAME faces GL does
     * (keeps back faces -> the dam wall etc. match; without this Metal culls the
     * opposite set). */
    /* CCW-front matches GL's glFrontFace(GL_CCW), so front-face culling removes the
     * SAME faces on both backends (verified: 0.000% shadow-map diff vs the GL front
     * cull). Front-face cull keeps back faces -> the standard shadow-acne mitigation. */
    [enc setFrontFacingWinding:MTLWindingCounterClockwise];
    [enc setCullMode:MTLCullModeFront];
    [enc setDepthBias:4.0f slopeScale:2.0f clamp:0.0f];
    /* GL_DEPTH_CLAMP (gfx_opengl.c:3734) is process-global GL state, so it is
     * already in effect during GL's shadow pass too. Metal's depth-clip mode is
     * per-encoder (default Clip), so without this call boundary casters get
     * near/far-clipped here where GL clamps them — mirror the scene encoder
     * (mtl_open_scene_encoder, :1184). GE007_NO_METAL_SHADOW_DEPTH_CLAMP=1
     * (negative control) skips it to reproduce the pre-fix clip. */
    if (!mtl_shadow_depth_clamp_disabled())
        [enc setDepthClipMode:MTLDepthClipModeClamp];
    [enc setVertexBuffer:vbuf offset:0 atIndex:0];
    [enc setVertexBytes:encode_mat length:16 * sizeof(float) atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
            vertexCount:(NSUInteger)(tri_count * 3)];
}

/* GE007_DUMP_SHADOW_MAP=1: render the shadow map into its own command buffer, blit
 * the Depth32Float target to a shared buffer, and write shadow_map.pgm once (local
 * diag only, R2). Non-blank assert in the trace. */
static void mtl_dump_shadow_pgm(id<MTLBuffer> vbuf, size_t tri_count, const float *sm16) {
    static int done = 0;
    if (done) return;
    static int want = -1;
    if (want < 0) want = (getenv("GE007_DUMP_SHADOW_MAP") != NULL);
    if (!want || s_shadow_depth == nil) return;
    done = 1;
    int res = s_shadow_res;
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.depthAttachment.texture = s_shadow_depth;
    rpd.depthAttachment.loadAction = MTLLoadActionClear;
    rpd.depthAttachment.clearDepth = 1.0;
    rpd.depthAttachment.storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
    mtl_shadow_encode(enc, vbuf, tri_count, sm16);
    [enc endEncoding];
    NSUInteger bpr = (NSUInteger)res * sizeof(float);
    id<MTLBuffer> buf = [s_device newBufferWithLength:bpr * (NSUInteger)res
                                             options:MTLResourceStorageModeShared];
    if (buf == nil) return;
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:s_shadow_depth sourceSlice:0 sourceLevel:0
          sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(res, res, 1)
              toBuffer:buf destinationOffset:0
     destinationBytesPerRow:bpr destinationBytesPerImage:bpr * (NSUInteger)res];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const float *depth = (const float *)buf.contents;
    float mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < res * res; i++) { if (depth[i] < mn) mn = depth[i]; if (depth[i] > mx) mx = depth[i]; }
    FILE *f = fopen("shadow_map.pgm", "wb");
    if (f) {
        fprintf(f, "P5\n%d %d\n255\n", res, res);
        /* FIXED normalization (raw depth -> gray), matching gfx_opengl's dump so the
         * GL and Metal PGMs are directly comparable (T3). Metal blit rows are
         * top-to-bottom; GL glGetTexImage is bottom-to-top — emit reversed to match. */
        for (int row = res - 1; row >= 0; row--) {
            for (int col = 0; col < res; col++) {
                int v = (int)(depth[row * res + col] * 255.0f + 0.5f);
                if (v < 0) v = 0; if (v > 255) v = 255;
                unsigned char c = (unsigned char)v;
                fwrite(&c, 1, 1, f);
            }
        }
        fclose(f);
    }
    fprintf(stderr, "[SHADOW_MAP] dumped shadow_map.pgm res=%d depth_min=%.5f depth_max=%.5f %s\n",
            res, mn, mx, (mx - mn > 1e-4f) ? "NON-BLANK-OK" : "BLANK-FAIL");
    fflush(stderr);
}

/* W1.E3.T3: replay the prior frame's captured casters into the shadow depth map,
 * encoded onto the frame's command buffer before the scene encoder opens. */
static void mtl_render_shadow_map(void) {
    /* §3.5 fix: default to "not ready" and only flip true after a full,
     * successful, non-empty replay below — mirrors gfx_opengl_render_shadow_map.
     * Every early-return path here (feature off/stale matrix, empty ring,
     * resource creation not yet complete, allocation failure) now leaves the
     * receiver gated off instead of sampling a mid-frame-created/uninitialized
     * private texture. */
    g_pc_shadow_map_ready = 0;
    if (!g_pcSunShadow || !g_pc_shadow_mat_valid) return;
    size_t tri_count = 0;
    const float *geom = gfx_shadow_get_geometry(&tri_count);
    if (geom == NULL || tri_count == 0) return;
    mtl_ensure_shadow_resources(g_pcSunShadowRes);
    if (s_shadow_depth == nil || s_shadow_pso == nil) return;

    /* Ring-buffered by s_vbuf_slot (set earlier this frame in mtl_start_frame,
     * before this function runs — see the call site) so this memcpy never
     * races an in-flight shadow pass still reading a prior frame's slot. */
    id<MTLBuffer> shadow_vbuf = s_shadow_vbuf[s_vbuf_slot];
    NSUInteger need = (NSUInteger)tri_count * 9 * sizeof(float);
    if (shadow_vbuf == nil || s_shadow_vbuf_cap[s_vbuf_slot] < need) {
        shadow_vbuf = [s_device newBufferWithLength:need options:MTLResourceStorageModeShared];
        s_shadow_vbuf[s_vbuf_slot] = shadow_vbuf;
        s_shadow_vbuf_cap[s_vbuf_slot] = (shadow_vbuf != nil) ? need : 0;
    }
    if (shadow_vbuf == nil) return;
    memcpy(shadow_vbuf.contents, geom, need);

    /* g_pc_shadow_mat is m[row][col] (column-vector M*v). MSL float4x4 is
     * column-major (M*v uses columns), so upload the transpose: sm[c*4+r] = M[r][c]. */
    float sm[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            sm[c * 4 + r] = g_pc_shadow_mat[r][c];

    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.depthAttachment.texture = s_shadow_depth;
    rpd.depthAttachment.loadAction = MTLLoadActionClear;
    rpd.depthAttachment.clearDepth = 1.0;
    rpd.depthAttachment.storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [s_cmdbuf renderCommandEncoderWithDescriptor:rpd];
    mtl_shadow_encode(enc, shadow_vbuf, tri_count, sm);
    [enc endEncoding];

    g_pc_shadow_map_ready = 1;
    mtl_dump_shadow_pgm(shadow_vbuf, tri_count, sm);
}

static void mtl_start_frame(void) {
  @autoreleasepool {  /* drains per-frame autoreleased temporaries — without it,
                       * the C game loop never drains and RSS/drawable pool grow. */
    if (s_layer == nil || s_queue == nil) return;
    s_frame_count_metal++;
    /* W3.E1: start a programmatic GPU capture of the target frame BEFORE its
     * command buffer is created, so the whole frame lands in the .gputrace. */
    {
        long cap = mtl_gpu_capture_target();
        if (!s_gpu_capturing && cap >= 0 && (long)s_frame_count_metal == cap)
            mtl_gpu_capture_begin();
    }
    /* Reset the SSAO scene-projection coefficient each frame (mirrors
     * gfx_opengl_start_frame): gfx_sp_matrix only ever RAISES proj_b, so without
     * this the `proj_b != 0` SSAO gate would latch on forever and menu/HUD frames
     * would feed stale coefficients. Render-only global — gameplay-invariant. */
    g_pc_ssao_proj_b = 0.0f;
    /* W3.E2 SSAO v2: reset the view-ray coefficients too (mirrors gfx_opengl
     * start_frame). gfx_sp_matrix only RAISES these at the main world view, so
     * without the reset a HUD/menu frame reuses the prior frame's x/y. */
    g_pc_ssao_proj_x = 0.0f;
    g_pc_ssao_proj_y = 0.0f;
    g_pc_view_inv_valid = 0;   /* W1.E2.T1: recapture the view-inverse each frame */
    /* Render at the frontend's resolution (gfx_current_dimensions), which the
     * viewports/T&L are computed against — NOT the raw layer size, which can be
     * a 2x render-scale/SSAA smaller. */
    int rw = (int)gfx_current_dimensions.width;
    int rh = (int)gfx_current_dimensions.height;
    if (rw <= 0 || rh <= 0) {
        CGSize ds = s_layer.drawableSize;
        rw = (int)ds.width;
        rh = (int)ds.height;
    }
    mtl_ensure_targets(rw, rh);
    mtl_ensure_white();
    if (s_scene_color == nil) {
        /* M-2 fix: this early-return skips mtl_render_shadow_map() below, which
         * is the only place that clears g_pc_shadow_map_ready for the frame.
         * Without an explicit clear here, a target-allocation failure leaves
         * the flag stale-true from a previous successful frame, and the
         * receiver would sample a depth map that was never refreshed this
         * frame. Mirror GL's unconditional per-frame clear
         * (gfx_opengl_render_shadow_map sets this to 0 before any of its own
         * early-return checks). */
        g_pc_shadow_map_ready = 0;
        return;
    }

    /* Throttle the CPU to <= MTL_VBUF_SLOTS frames ahead so the ring slot we are
     * about to write is not still being read by the GPU. Placed AFTER the early
     * returns and immediately before the command buffer is created, so this wait
     * is exactly paired with the signal in end_frame (which runs iff s_cmdbuf). */
    if (s_frame_sem != nil) dispatch_semaphore_wait(s_frame_sem, DISPATCH_TIME_FOREVER);
    s_vbuf_slot = s_ring_index % MTL_VBUF_SLOTS;
    s_ring_index++;
    s_vbuf_cursor = 0;
    if (s_vbuf[s_vbuf_slot] == nil || s_vbuf_cap[s_vbuf_slot] < s_vbuf_want) {
        /* Safe to (re)allocate: the semaphore guarantees this slot's prior GPU
         * work completed. Any still-in-flight reference to the old buffer is
         * retained by its command buffer until the GPU is done. */
        s_vbuf[s_vbuf_slot] = [s_device newBufferWithLength:s_vbuf_want options:MTLResourceStorageModeShared];
        s_vbuf_cap[s_vbuf_slot] = (s_vbuf[s_vbuf_slot] != nil) ? s_vbuf_want : 0;
    }

    /* W3.E1 GPU timers (diagnostic; no-op unless GE007_METAL_GPU_TRACE). Reserve
     * this frame's slot window in a per-ring-slot region of the counter buffer so
     * an in-flight completed-handler never races the next frame's writes (same
     * discipline as the vbuf ring). base+0 = whole-frame START (attached to the
     * scene pass, below); base+1.. = post-pass start/end pairs. Must run BEFORE
     * mtl_open_scene_encoder so the scene pass can attach the frame-start ts. */
    if (mtl_gpu_trace_on()) {
        mtl_gpu_ts_ensure();
        int base = s_vbuf_slot * MTL_GPU_TS_PER_FRAME;
        s_frame_ts_slot = base;
        s_frame_ts_written = 0;
        s_post_ts_next  = base + 1;
        s_post_ts_limit = base + MTL_GPU_TS_PER_FRAME;
        s_post_ts_first = -1;
        s_post_ts_last  = -1;
        s_sp_scene = os_signpost_id_generate(mtl_signpost_log());
        os_signpost_interval_begin(mtl_signpost_log(), s_sp_scene, "scene");
    }

    s_cmdbuf = [s_queue commandBuffer];
    if (s_cmdbuf == nil) {
        /* GPU reset / device-lost / VRAM exhaustion: the queue handed back no command
         * buffer. end_frame bails on a nil s_cmdbuf before its addCompletedHandler runs,
         * so the frame-throttle permit taken by the dispatch_semaphore_wait above would
         * never be returned — after MTL_VBUF_SLOTS such frames the next wait blocks
         * forever and the game hard-freezes. Return the permit and skip this frame so the
         * backend can recover once the GPU is back. */
        if (s_frame_sem != nil) dispatch_semaphore_signal(s_frame_sem);
        return;
    }
    /* W1.E3.T3: replay the prior frame's captured casters into the sun-shadow depth
     * map before the scene encoder opens (capture-and-replay, §4.5). The ring is
     * reset in gfx_pc.c after start_frame, preserving the one-frame latency. */
    mtl_render_shadow_map();
    mtl_open_scene_encoder(true);

    if (!s_logged_first_frame) {
        fprintf(stderr, "[metal] first frame: scene %dx%d, geometry encoder open\n", s_fb_w, s_fb_h);
        s_logged_first_frame = true;
    }
  }
}

/* ==========================================================================
 * Phase 4 — output-VI-filter post-FX (FXAA / bloom / grade / tonemap / gamma /
 * vignette / CAS sharpen / Bayer dither / RGB555). Fullscreen-triangle chain run
 * in end_frame, sampling s_scene_color. Ports gfx_opengl.c:2833-3091 (minus SSAO,
 * which lands in Phase 5) + the per-pass uniform values (gfx_opengl.c:3163-3244).
 * ========================================================================== */

/* C mirror of the MSL `struct FilterUniforms` — 16-byte aligned, 144 bytes.
 * Field offsets match the MSL struct (float4@0, float2@16/24, 17 floats@32,
 * 10 ints@100: applyPost..fbH then ssaoMode); one trailing pad rounds MSL's
 * align-16 size to 144. ssaoMode (W3.E2) consumed one of the original two pads
 * so the struct stays 144 bytes — keep the MSL twin field in the same slot.
 *   ssaoMode: 0=none, 2=hemisphere-v2 composite, 3=hemisphere-v2 debug (raw AO). */
struct MtlFilterUniforms {
    float colorTint[4];     /* 0  (float4; .xyz used) */
    float srcSize[2];       /* 16 */
    float dstSize[2];       /* 24 */
    float colorScale, colorBias, gamma, saturation, contrast, brightness, vignette,
          bloomThreshold, bloomIntensity, sharpen, levelSat, levelCon,
          ssaoRadius, ssaoIntensity, ssaoAspect, ssaoProjA, ssaoProjB;  /* 32..99 */
    int applyPost, dither, bloom, ssao, filterMode, fxaa, tonemap, rgb555, fbH,
        ssaoMode;           /* 100..139 */
    int linearFinish;       /* 140..143 -> 144 */
};

static id<MTLLibrary>            s_filter_lib = nil;
static id<MTLRenderPipelineState> s_filter_pso = nil;
static id<MTLSamplerState>       s_filter_smp = nil;

/* SSAO v2 (W3.E2) — PASS A (hemisphere) + PASS B (bilateral blur) programs.
 * C mirror of the MSL `struct SsaoUniforms` — 16-byte aligned, 64 bytes. */
struct MtlSsaoUniforms {
    float projA, projB, projX, projY;      /* g_pc_ssao_proj_* (view-space recon)   */
    float radius;        /* view-space units: g_pcSsaoRadius * kRadiusWorldScale     */
    float bias;          /* g_pcSsaoBias — depth-proportional self-occlusion reject  */
    float power;         /* g_pcSsaoPower — AO contrast exponent                     */
    float farCutoff;     /* g_pcSsaoFarCutoff — view-Z where AO fades to 0           */
    float nearCut, skyCut;                 /* g_pcSsaoNearCut / g_pcSsaoSkyCut         */
    float blurDepthSharp;                  /* g_pcSsaoBlurDepthSharp (PASS B)          */
    float aoSizeW, aoSizeH;                /* AO target size (half-res aware)          */
    int   frameParity;   /* reserved 0 — no temporal accumulation in v2              */
    int   blurAxis;      /* 0 = X pass, 1 = Y pass                                   */
    int   _pad;
};
static id<MTLLibrary>             s_ssao_lib = nil;
static id<MTLRenderPipelineState> s_ssao_pso = nil;       /* PASS A hemisphere        */
static id<MTLRenderPipelineState> s_ssao_blur_pso = nil;  /* PASS B bilateral (T3)    */

static float mtl_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* GE007_SSAO_DEBUG=1 (W3.E2.T2): the final filter pass returns the raw AO field
 * (float4(float3(ao),1)) instead of compositing it — for the ROI capture gates. */
static int mtl_ssao_debug(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("GE007_SSAO_DEBUG"); m = (e && (e[0]=='1'||e[0]=='o'||e[0]=='t')) ? 1 : 0; }
    return m;
}

/* Diag knobs (default identity) — cached like the other env checks. */
static float mtl_diag_color_scale(void) {
    static float v = -1.0f;
    if (v < 0.0f) { const char *e = getenv("GE007_DIAG_OUTPUT_COLOR_SCALE"); v = (e && e[0]) ? (float)atof(e) : 1.0f; }
    return v;
}
static float mtl_diag_color_bias(void) {
    static int checked = 0; static float v = 0.0f;
    if (!checked) { const char *e = getenv("GE007_DIAG_OUTPUT_COLOR_BIAS"); if (e && e[0]) v = (float)atof(e); checked = 1; }
    return v;
}
static int mtl_diag_rgb555_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("GE007_DIAG_OUTPUT_RGB555");
        m = 0;
        if (e && e[0]) {
            if (!strcmp(e, "1") || !strcmp(e, "on") || !strcmp(e, "true")) m = 1;
            else if (!strcmp(e, "dither")) m = 2;
        }
    }
    return m;
}

/* Gate mirrors gfx_opengl_output_color_adjust_active (gfx_opengl.c:2779-2808)
 * plus the separate bloom term from apply_output_vi_filter's gate (:3289-3295).
 * VI-downscale + SSAO terms are out of Phase-4 scope (single-pass, SSAO off). */
static bool mtl_output_filter_active(void) {
    float gamma = mtl_clampf(g_pcVideoGamma, 0.5f, 2.5f);
    /* gamma + diag color/rgb555 are honored even with RemasterFX off (display). */
    if (mtl_diag_color_scale() != 1.0f || mtl_diag_color_bias() != 0.0f ||
        mtl_diag_rgb555_mode() != 0 || gamma < 0.999f || gamma > 1.001f) return true;
    if (!g_pcRemasterFX) return false;
    return mtl_clampf(g_pcVideoSaturation, 0.0f, 2.0f) != 1.0f ||
           mtl_clampf(g_pcVideoContrast, 0.5f, 2.0f) != 1.0f ||
           mtl_clampf(g_pcVideoBrightness, -0.5f, 0.5f) != 0.0f ||
           g_pcOutputDither != 0 ||
           mtl_clampf(g_pcVignette, 0.0f, 1.0f) > 0.0001f ||
           g_pcFxaa != 0 ||
           /* SMAA (W3.E4) runs before the filter and feeds passes_src[0]; the
            * filter must run so s_smaa_out reaches the drawable. Gated on
            * RemasterFX like FXAA (we are past the !RemasterFX early-out). */
           g_pcSmaa != 0 ||
           mtl_clampf(g_pcSharpen, 0.0f, 1.0f) > 0.0f ||   /* GL's use_sharpen gate is >0 (:3293) */
           (g_pcTonemap != 0 && g_pcGradeLevelValid) ||
           g_pcBloom != 0 ||
           /* SSAO alone must trigger the filter chain (mirrors GL's use_ssao,
            * gfx_opengl.c:3292 — output_ssao_active is RemasterFX && Ssao, and we
            * are already past the !RemasterFX early-out; proj_b!=0 stands in for
            * GL's g_scene_depth_valid on the single-sample Metal path). */
           (g_pcSsao != 0 && g_pc_ssao_proj_b != 0.0f) ||
           (g_pcGradePresets && g_pcGradeLevelValid &&
            (g_pcGradeLevelSat != 1.0f || g_pcGradeLevelCon != 1.0f ||
                                 g_pcGradeLevelTintR != 1.0f || g_pcGradeLevelTintG != 1.0f ||
                                 g_pcGradeLevelTintB != 1.0f));
}

/* Build + compile the fullscreen filter library/PSO/sampler once. Returns false
 * (caller falls back to the straight blit) on any failure. */
static bool mtl_ensure_filter_program(void) {
    if (s_filter_pso != nil) return true;
    if (s_device == nil) return false;
    TB s; tb_init(&s, 32768);
    tb_str(&s, "#include <metal_stdlib>\nusing namespace metal;\n");
    tb_str(&s, "struct FilterUniforms {\n");
    tb_str(&s, "  float4 colorTint; float2 srcSize; float2 dstSize;\n");
    tb_str(&s, "  float colorScale, colorBias, gamma, saturation, contrast, brightness, vignette, bloomThreshold, bloomIntensity, sharpen, levelSat, levelCon, ssaoRadius, ssaoIntensity, ssaoAspect, ssaoProjA, ssaoProjB;\n");
    tb_str(&s, "  int applyPost, dither, bloom, ssao, filterMode, fxaa, tonemap, rgb555, fbH, ssaoMode, linearFinish;\n");
    tb_str(&s, "};\n");
    tb_str(&s, "constant float kBayer4[16] = { 0.0/16.0, 8.0/16.0, 2.0/16.0, 10.0/16.0, 12.0/16.0, 4.0/16.0, 14.0/16.0, 6.0/16.0, 3.0/16.0, 11.0/16.0, 1.0/16.0, 9.0/16.0, 15.0/16.0, 7.0/16.0, 13.0/16.0, 5.0/16.0 };\n");
    tb_str(&s, "struct FVO { float4 position [[position]]; float2 vTexCoord; };\n");
    /* Fullscreen triangle; V-flip vTexCoord so v=0 is the TOP of the top-left
     * scene texture (bloom/vignette sample via vTexCoord). */
    tb_str(&s, "vertex FVO filterVS(uint vid [[vertex_id]]) {\n");
    tb_str(&s, "  const float2 kPos[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n");
    tb_str(&s, "  FVO o; o.position = float4(kPos[vid], 0.0, 1.0);\n");
    tb_str(&s, "  o.vTexCoord = float2(kPos[vid].x * 0.5 + 0.5, 0.5 - kPos[vid].y * 0.5); return o;\n}\n");
    /* Color helpers (thread tex+uniforms explicitly — MSL free funcs see no globals).
     * texelFetch -> uTex.read(uint2) with GL's clamp preserved. */
    tb_str(&s, "static float4 sampleNearest(texture2d<float> uTex, constant FilterUniforms& u, float2 d) {\n");
    tb_str(&s, "  int2 p = int2(floor(d * u.srcSize / u.dstSize)); p = clamp(p, int2(0), int2(u.srcSize) - int2(1)); return uTex.read(uint2(p));\n}\n");
    tb_str(&s, "static float2 fitSizeForAspect(float2 b, float a) { float ba = b.x / b.y; if (ba > a) return float2(b.y * a, b.y); return float2(b.x, b.x / a); }\n");
    tb_str(&s, "static float4 sampleFitSrcToDst(texture2d<float> uTex, constant FilterUniforms& u, float2 d) {\n");
    tb_str(&s, "  float sa = u.srcSize.x / u.srcSize.y; float2 fs = fitSizeForAspect(u.dstSize, sa); float2 off = floor((u.dstSize - fs) * 0.5);\n");
    tb_str(&s, "  if (d.x < off.x || d.y < off.y || d.x >= off.x + fs.x || d.y >= off.y + fs.y) return float4(0.0,0.0,0.0,1.0);\n");
    tb_str(&s, "  int2 p = int2(floor((d - off) * u.srcSize / fs)); p = clamp(p, int2(0), int2(u.srcSize) - int2(1)); return uTex.read(uint2(p));\n}\n");
    tb_str(&s, "static float4 sampleFitLogical(texture2d<float> uTex, constant FilterUniforms& u, float2 d) {\n");
    tb_str(&s, "  float da = u.dstSize.x / u.dstSize.y; float2 fs = fitSizeForAspect(u.srcSize, da); float2 off = floor((u.srcSize - fs) * 0.5);\n");
    tb_str(&s, "  int2 p = int2(floor(off + d * fs / u.dstSize)); p = clamp(p, int2(0), int2(u.srcSize) - int2(1)); return uTex.read(uint2(p));\n}\n");
    tb_str(&s, "static float4 sampleCpuBilinear(texture2d<float> uTex, constant FilterUniforms& u, float2 d) {\n");
    tb_str(&s, "  float2 sc = d * u.srcSize / u.dstSize - float2(0.5); int2 p0 = int2(floor(sc)); float2 f = sc - float2(p0);\n");
    tb_str(&s, "  if (p0.x < 0) { p0.x = 0; f.x = 0.0; } else if (p0.x >= int(u.srcSize.x) - 1) { p0.x = int(u.srcSize.x) - 1; f.x = 0.0; }\n");
    tb_str(&s, "  if (p0.y < 0) { p0.y = 0; f.y = 0.0; } else if (p0.y >= int(u.srcSize.y) - 1) { p0.y = int(u.srcSize.y) - 1; f.y = 0.0; }\n");
    tb_str(&s, "  int2 p1 = min(p0 + int2(1), int2(u.srcSize) - int2(1));\n");
    tb_str(&s, "  float4 c00 = uTex.read(uint2(p0)); float4 c10 = uTex.read(uint2(int2(p1.x, p0.y)));\n");
    tb_str(&s, "  float4 c01 = uTex.read(uint2(int2(p0.x, p1.y))); float4 c11 = uTex.read(uint2(p1));\n");
    tb_str(&s, "  return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);\n}\n");
    tb_str(&s, "static float4 sampleDst(texture2d<float> uTex, constant FilterUniforms& u, float2 d) {\n");
    tb_str(&s, "  if (u.filterMode == 1) return sampleNearest(uTex, u, d);\n");
    tb_str(&s, "  else if (u.filterMode == 2) return sampleFitSrcToDst(uTex, u, d);\n");
    tb_str(&s, "  else if (u.filterMode == 3) return sampleFitLogical(uTex, u, d);\n");
    tb_str(&s, "  return sampleCpuBilinear(uTex, u, d);\n}\n");
    tb_str(&s, "static float fxLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n");
    tb_str(&s, "static float3 srgbToLinear(float3 value) {\n");
    tb_str(&s, "  float3 c = clamp(value, 0.0, 1.0); return select(c / 12.92, pow((c + 0.055) / 1.055, float3(2.4)), c >= 0.04045);\n}\n");
    tb_str(&s, "static float3 linearToSrgb(float3 value) {\n");
    tb_str(&s, "  float3 c = max(value, 0.0); return select(c * 12.92, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, c >= 0.0031308);\n}\n");
    tb_str(&s, "static float3 filmicFinish(float3 c) {\n");
    tb_str(&s, "  float3 shoulder = c * 1.18 / (float3(1.0) + c * 0.18); return mix(c, shoulder, 0.20);\n}\n");
    tb_str(&s, "static float3 fxaa(texture2d<float> uTex, constant FilterUniforms& u, float2 fc, float3 rgbM) {\n");
    tb_str(&s, "  float lM = fxLuma(rgbM);\n");
    tb_str(&s, "  float lN = fxLuma(sampleDst(uTex,u,fc+float2(0.0,-1.0)).rgb); float lS = fxLuma(sampleDst(uTex,u,fc+float2(0.0,1.0)).rgb);\n");
    tb_str(&s, "  float lW = fxLuma(sampleDst(uTex,u,fc+float2(-1.0,0.0)).rgb); float lE = fxLuma(sampleDst(uTex,u,fc+float2(1.0,0.0)).rgb);\n");
    tb_str(&s, "  float lNW = fxLuma(sampleDst(uTex,u,fc+float2(-1.0,-1.0)).rgb); float lNE = fxLuma(sampleDst(uTex,u,fc+float2(1.0,-1.0)).rgb);\n");
    tb_str(&s, "  float lSW = fxLuma(sampleDst(uTex,u,fc+float2(-1.0,1.0)).rgb); float lSE = fxLuma(sampleDst(uTex,u,fc+float2(1.0,1.0)).rgb);\n");
    tb_str(&s, "  float lMin = min(lM, min(min(lN,lS), min(lW,lE))); float lMax = max(lM, max(max(lN,lS), max(lW,lE)));\n");
    tb_str(&s, "  float range = lMax - lMin; if (range < max(0.0625, lMax * 0.125)) return rgbM;\n");
    tb_str(&s, "  float2 dir; dir.x = -((lNW + lNE) - (lSW + lSE)); dir.y = ((lNW + lSW) - (lNE + lSE));\n");
    tb_str(&s, "  float dirReduce = max((lNW+lNE+lSW+lSE) * 0.03125, 0.0078125); float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n");
    tb_str(&s, "  dir = clamp(dir * rcpDirMin, float2(-8.0), float2(8.0));\n");
    tb_str(&s, "  float3 rgbA = 0.5 * (sampleDst(uTex,u,fc + dir * (1.0/3.0 - 0.5)).rgb + sampleDst(uTex,u,fc + dir * (2.0/3.0 - 0.5)).rgb);\n");
    tb_str(&s, "  float3 rgbB = rgbA * 0.5 + 0.25 * (sampleDst(uTex,u,fc + dir * -0.5).rgb + sampleDst(uTex,u,fc + dir * 0.5).rgb);\n");
    tb_str(&s, "  float lB = fxLuma(rgbB); if (lB < lMin || lB > lMax) return rgbA; return rgbB;\n}\n");
    tb_str(&s, "static float3 casSharpen(texture2d<float> uTex, constant FilterUniforms& u, float2 fc, float3 rgbC) {\n");
    tb_str(&s, "  float3 n = sampleDst(uTex,u,fc+float2(0.0,-1.0)).rgb; float3 so = sampleDst(uTex,u,fc+float2(0.0,1.0)).rgb;\n");
    tb_str(&s, "  float3 w = sampleDst(uTex,u,fc+float2(-1.0,0.0)).rgb; float3 e = sampleDst(uTex,u,fc+float2(1.0,0.0)).rgb;\n");
    tb_str(&s, "  float3 mn = min(rgbC, min(min(n,so), min(w,e))); float3 mx = max(rgbC, max(max(n,so), max(w,e)));\n");
    tb_str(&s, "  float3 amp = clamp(min(mn, 1.0 - mx) / max(mx, 0.0001), 0.0, 1.0); amp = sqrt(amp);\n");
    tb_str(&s, "  float peak = -0.125 - 0.075 * u.sharpen; float3 wgt = amp * peak;\n");
    tb_str(&s, "  float3 sum = rgbC + (n + so + w + e) * wgt; float3 rcpW = 1.0 / (1.0 + 4.0 * wgt);\n");
    tb_str(&s, "  float3 outc = clamp(sum * rcpW, mn, mx); return mix(rgbC, outc, clamp(u.sharpen, 0.0, 1.0));\n}\n");
    /* SSAO (Phase 5) — ports gfx_opengl.c:2984-3018. Reads native Depth32Float
     * (the op that hangs GL-over-Metal). depth2d.sample returns a float (no .r).
     * ssaoLinZ reuses GL's 2d-1 window->NDC mapping verbatim: the frontend's
     * z=(z+w)/2 remap makes Metal window depth == GL window depth. */
    tb_str(&s, "constant float2 kSsaoDir[8] = { float2(1.0,0.0), float2(0.7071,0.7071), float2(0.0,1.0), float2(-0.7071,0.7071), float2(-1.0,0.0), float2(-0.7071,-0.7071), float2(0.0,-1.0), float2(0.7071,-0.7071) };\n");
    tb_str(&s, "static float ssaoLinZ(float d, constant FilterUniforms& u) { return u.ssaoProjB / (u.ssaoProjA + 2.0 * d - 1.0); }\n");
    tb_str(&s, "static float ssaoAO(depth2d<float> uDepthTex, sampler depSmp, constant FilterUniforms& u, float2 uv) {\n");
    tb_str(&s, "  float cd = uDepthTex.sample(depSmp, uv); if (cd >= 0.99999) return 0.0;\n");
    tb_str(&s, "  float cz = ssaoLinZ(cd, u); float occ = 0.0;\n");
    tb_str(&s, "  for (int i = 0; i < 8; ++i) {\n");
    tb_str(&s, "    float2 dir = kSsaoDir[i]; dir.x /= max(u.ssaoAspect, 0.001);\n");
    tb_str(&s, "    for (int sp = 1; sp <= 2; ++sp) {\n");
    tb_str(&s, "      float2 o = dir * u.ssaoRadius * float(sp);\n");
    tb_str(&s, "      float nz = ssaoLinZ(uDepthTex.sample(depSmp, uv + o), u); float diff = cz - nz;\n");
    tb_str(&s, "      if (diff > cz * 0.015 && diff < cz * 0.12) occ += 1.0 / float(sp);\n    }\n  }\n");
    tb_str(&s, "  return occ / 12.0;\n}\n");
    /* Main — orientation: sampleDst/fxaa/casSharpen use UNFLIPPED in.position
     * (scene + drawable are both top-left; self-consistent like GL's bottom-up);
     * bloom/vignette use the V-flipped vTexCoord; dither/rgb555 reconstruct GL's
     * bottom-left pixel index (fbH - position.y) for pixel-exact pattern parity. */
    tb_str(&s, "fragment float4 filterFragment(FVO in [[stage_in]], constant FilterUniforms& u [[buffer(1)]], texture2d<float> uTex [[texture(0)]], sampler colSmp [[sampler(0)]], depth2d<float> uDepthTex [[texture(1)]], sampler depSmp [[sampler(1)]], texture2d<float> uSsaoTex [[texture(2)]], sampler ssaoSmp [[sampler(2)]]) {\n");
    tb_str(&s, "  float4 color = sampleDst(uTex, u, in.position.xy);\n");
    /* SSAO v2 (W3.E2): AO is precomputed in s_ssao_raw by PASS A/B; the linear
     * ssaoSmp upsamples half-res AO. ssaoMode==3 is the GE007_SSAO_DEBUG raw-field
     * capture (returns before any post-FX so the ROI gates measure pure AO). */
    tb_str(&s, "  if (u.ssaoMode == 3) { return float4(float3(uSsaoTex.sample(ssaoSmp, in.vTexCoord).r), 1.0); }\n");
    tb_str(&s, "  if (u.applyPost == 1 && u.fxaa == 1) color.rgb = fxaa(uTex, u, in.position.xy, color.rgb);\n");
    tb_str(&s, "  float3 rgb = clamp(color.rgb * u.colorScale + float3(u.colorBias / 255.0), 0.0, 1.0);\n");
    tb_str(&s, "  if (u.linearFinish == 1) {\n");
    tb_str(&s, "    rgb = srgbToLinear(rgb);\n");
    tb_str(&s, "    if (u.ssao == 1) { float ao = 1.0 - u.ssaoIntensity * ssaoAO(uDepthTex, depSmp, u, in.vTexCoord); rgb *= clamp(ao, 0.0, 1.0); }\n");
    tb_str(&s, "    if (u.ssaoMode == 2) { float aoV = uSsaoTex.sample(ssaoSmp, in.vTexCoord).r; rgb *= clamp(1.0 - u.ssaoIntensity * (1.0 - aoV), 0.0, 1.0); }\n");
    tb_str(&s, "    if (u.bloom == 1) {\n");
    tb_str(&s, "      float2 texel = 1.0 / u.srcSize; float3 bloom = float3(0.0); float wsum = 0.0; const int R = 3;\n");
    tb_str(&s, "      for (int y = -R; y <= R; ++y) for (int x = -R; x <= R; ++x) {\n");
    tb_str(&s, "        float2 o = float2(float(x), float(y)) * texel * 2.0; float3 sp = srgbToLinear(uTex.sample(colSmp, in.vTexCoord + o).rgb);\n");
    tb_str(&s, "        float l = dot(sp, float3(0.2126, 0.7152, 0.0722)); float b = max(l - u.bloomThreshold, 0.0) / max(1.0 - u.bloomThreshold, 0.001);\n");
    tb_str(&s, "        float wv = exp(-float(x*x + y*y) / 6.0); bloom += sp * b * wv; wsum += wv;\n      }\n");
    tb_str(&s, "      rgb += bloom / max(wsum, 0.001) * u.bloomIntensity;\n    }\n");
    tb_str(&s, "    rgb += float3(u.brightness); float con = u.contrast * u.levelCon; rgb = (rgb - 0.18) * con + 0.18;\n");
    tb_str(&s, "    float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722)); float sat = u.saturation * u.levelSat; rgb = mix(float3(luma), rgb, sat);\n");
    tb_str(&s, "    rgb *= u.colorTint.rgb;\n");
    tb_str(&s, "    if (u.tonemap == 1) rgb = filmicFinish(max(rgb, 0.0));\n");
    tb_str(&s, "    rgb = linearToSrgb(clamp(rgb, 0.0, 1.0));\n  }\n");
    tb_str(&s, "  rgb = pow(rgb, float3(1.0 / max(u.gamma, 0.001)));\n");
    tb_str(&s, "  if (u.applyPost == 1 && u.vignette > 0.0) { float2 vc = in.vTexCoord - float2(0.5); float dd = dot(vc, vc) * 2.0; float vig = 1.0 - u.vignette * smoothstep(0.3, 1.0, dd); rgb *= vig; }\n");
    tb_str(&s, "  if (u.applyPost == 1 && u.sharpen > 0.0) rgb = casSharpen(uTex, u, in.position.xy, rgb);\n");
    tb_str(&s, "  if (u.applyPost == 1 && u.dither == 1) {\n");
    tb_str(&s, "    int dx = int(floor(in.position.x)) & 3; int dy = int(floor(float(u.fbH) - in.position.y)) & 3;\n");
    tb_str(&s, "    float t = kBayer4[dy * 4 + dx] - 0.5; rgb += float3(t / 255.0); rgb = clamp(rgb, 0.0, 1.0);\n  }\n");
    tb_str(&s, "  if (u.rgb555 != 0) {\n");
    tb_str(&s, "    float threshold = 0.5;\n");
    tb_str(&s, "    if (u.rgb555 == 2) { int dx = int(floor(in.position.x)) & 3; int dy = int(floor(float(u.fbH) - in.position.y)) & 3; threshold += kBayer4[dy * 4 + dx] - 0.5; }\n");
    tb_str(&s, "    rgb = floor(clamp(rgb, 0.0, 1.0) * 31.0 + threshold) / 31.0; rgb = clamp(rgb, 0.0, 1.0);\n  }\n");
    tb_str(&s, "  return float4(rgb, color.a);\n}\n");

  @autoreleasepool {  /* MSL compile transients (M3.2) */
    NSError *err = nil;
    NSString *nssrc = [NSString stringWithUTF8String:s.p];
    /* Prefer SAFE math so the rgb555/dither quantize boundaries stay bit-stable
     * vs GL (fast math may fuse/reassociate the floor(x*31+t) quantize). The
     * fastMathEnabled property is deprecated (macOS 15) — use mathMode where the
     * SDK supports it; older SDKs/runtimes keep the default (a sub-LSB nicety). */
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    if (@available(macOS 15.0, *)) {
        opts.mathMode = MTLMathModeSafe;
    }
#endif
    s_filter_lib = [s_device newLibraryWithSource:nssrc options:opts error:&err];
    tb_free(&s);
    if (s_filter_lib == nil) {
        fprintf(stderr, "[metal] output-filter MSL compile failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
        return false;
    }
    id<MTLFunction> vfn = [s_filter_lib newFunctionWithName:@"filterVS"];
    id<MTLFunction> ffn = [s_filter_lib newFunctionWithName:@"filterFragment"];
    if (vfn == nil || ffn == nil) return false;

    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vfn;
    pd.fragmentFunction = ffn;
    pd.vertexDescriptor = nil;                 /* fullscreen triangle via [[vertex_id]] */
    pd.rasterSampleCount = 1;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.colorAttachments[0].blendingEnabled = NO;
    s_filter_pso = [s_device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (s_filter_pso == nil) {
        fprintf(stderr, "[metal] output-filter PSO build failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
        return false;
    }
    MTLSamplerDescriptor *smp = [[MTLSamplerDescriptor alloc] init];
    smp.minFilter = MTLSamplerMinMagFilterLinear;
    smp.magFilter = MTLSamplerMinMagFilterLinear;
    smp.mipFilter = MTLSamplerMipFilterNotMipmapped;
    smp.sAddressMode = MTLSamplerAddressModeClampToEdge;
    smp.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_filter_smp = [s_device newSamplerStateWithDescriptor:smp];
    return s_filter_pso != nil && s_filter_smp != nil;
  }
}

/* Fill the filter uniforms for one pass, mirroring the per-pass values in
 * gfx_opengl_draw_output_filter_texture:3176-3241. `apply_post` is GL's pass
 * argument (0 = pre-pass, 1 = final pass). colorScale/colorBias/gamma are set
 * regardless of apply_post (GL applies them in BOTH passes — see the 2-pass note
 * on mtl_run_output_filter); everything else is apply_post-gated exactly as GL.
 * SSAO fields are zeroed (Phase 5). src == dst == scene (mode 0). */
static void mtl_fill_filter_uniforms(struct MtlFilterUniforms *u, int apply_post) {
    memset(u, 0, sizeof *u);
    int gp =
        (g_pcGradePresets && g_pcGradeLevelValid) ? 1 : 0;
    u->colorTint[0] = gp ? g_pcGradeLevelTintR : 1.0f;
    u->colorTint[1] = gp ? g_pcGradeLevelTintG : 1.0f;
    u->colorTint[2] = gp ? g_pcGradeLevelTintB : 1.0f;
    u->colorTint[3] = 1.0f;
    u->srcSize[0] = (float)s_fb_w; u->srcSize[1] = (float)s_fb_h;
    u->dstSize[0] = (float)s_fb_w; u->dstSize[1] = (float)s_fb_h;
    u->colorScale = mtl_diag_color_scale();
    u->colorBias = mtl_diag_color_bias();
    u->gamma = mtl_clampf(g_pcVideoGamma, 0.5f, 2.5f);
    u->saturation = mtl_clampf(g_pcVideoSaturation, 0.0f, 2.0f);
    u->contrast = mtl_clampf(g_pcVideoContrast, 0.5f, 2.0f);
    u->brightness = mtl_clampf(g_pcVideoBrightness, -0.5f, 0.5f);
    u->vignette = mtl_clampf(g_pcVignette, 0.0f, 1.0f);
    u->bloomThreshold = g_pcBloomThreshold;
    u->bloomIntensity = g_pcBloomIntensity;
    u->sharpen = apply_post ? mtl_clampf(g_pcSharpen, 0.0f, 1.0f) : 0.0f;
    u->levelSat = gp ? g_pcGradeLevelSat : 1.0f;
    u->levelCon = gp ? g_pcGradeLevelCon : 1.0f;
    u->applyPost = (apply_post && g_pcRemasterFX) ? 1 : 0;
    u->dither = g_pcOutputDither ? 1 : 0;   /* shader gates on applyPost */
    u->bloom = g_pcBloom ? 1 : 0;           /* shader gates on applyPost */
    /* SSAO: final pass only, gated like gfx_opengl.c:3201 (apply_post && Ssao &&
     * proj_b != 0; RemasterFX implied by apply_post). No MSAA clause — Metal is
     * single-sample here, so the GL "SSAO off under MSAA" limit is gone.
     *   mode 1 (planar/v1): inline ssaoAO() as before (u->ssao=1).
     *   mode 2 (hemisphere/v2): AO precomputed in s_ssao_raw; composite samples it
     *     (u->ssaoMode=2), or return the raw field under GE007_SSAO_DEBUG (=3). */
    /* FID-0018: SSAO is disabled under MSAA (GL contract — GL's "SSAO off under
     * MSAA" limit). The scene pass writes depth into the multisample attachment,
     * so the single-sample s_scene_depth this pass samples is stale; rather than
     * add a depth resolve, mirror GL and skip AO while MSAA is on. No effect on
     * the default (MSAA off) path. */
    int ssaoActive = (apply_post && g_pcRemasterFX && g_pcSsao != 0 &&
                      g_pc_ssao_proj_b != 0.0f && !mtl_msaa_active());
    u->ssao = (ssaoActive && g_pcSsaoMode != 2) ? 1 : 0;
    u->ssaoMode = (ssaoActive && g_pcSsaoMode == 2) ? (mtl_ssao_debug() ? 3 : 2) : 0;
    u->ssaoRadius = g_pcSsaoRadius * 0.02f;  /* radius key -> UV offset scale (load-bearing) */
    u->ssaoIntensity = g_pcSsaoIntensity;
    u->ssaoAspect = s_fb_h > 0 ? (float)s_fb_w / (float)s_fb_h : 1.0f;
    u->ssaoProjA = g_pc_ssao_proj_a;
    u->ssaoProjB = g_pc_ssao_proj_b;
    /* Mode 0 (bilinear) is the only mode used here; src == dst makes it an
     * identity fetch, and its centered kernel commutes with the top-left/
     * bottom-left flip so the unflipped in.position is byte-exact. Modes 1/2/3
     * (nearest / aspect-fit, for the deferred VI-downscale 2-pass) do NOT commute
     * with the flip — when that feature lands, feed them a bottom-left dst coord
     * (dstSize.y - position.y) and read the mirrored src row, or the letterbox
     * bar lands on the wrong edge and content shifts a row vs GL. */
    u->filterMode = 0;
    /* SMAA and FXAA are mutually exclusive edge-AA passes: when SMAA is on the
     * scene is already anti-aliased in s_smaa_out (fed to passes_src[0]), so the
     * filter's FXAA branch is forced off. One-time log when both were requested. */
    if (apply_post && g_pcFxaa && g_pcSmaa) {
        static bool logged = false;
        if (!logged) { fprintf(stderr, "[metal] SMAA on: FXAA disabled (mutually exclusive)\n"); logged = true; }
    }
    u->fxaa = (apply_post && g_pcFxaa && g_pcSmaa == 0) ? 1 : 0;
    u->tonemap =
        (apply_post && g_pcTonemap && g_pcGradeLevelValid) ? 1 : 0;
    u->linearFinish =
        (apply_post && g_pcRemasterFX &&
         (u->tonemap != 0 || gp != 0 || u->bloom != 0 ||
          ssaoActive || u->saturation != 1.0f ||
          u->contrast != 1.0f || u->brightness != 0.0f))
            ? 1 : 0;
    u->rgb555 = apply_post ? mtl_diag_rgb555_mode() : 0;
    u->fbH = s_fb_h;
}

/* Run the output filter as GL's TWO passes (gfx_opengl_apply_output_vi_filter is
 * a resolve pre-pass + a final pass even without VI downscale): pass 1 applies
 * colorScale/bias/gamma into an 8-bit intermediate (s_filter_low, apply_post=0);
 * pass 2 reads that and applies the full post-FX into s_final_color
 * (apply_post=1). Because colorScale/bias/gamma live OUTSIDE GL's apply_post
 * guard, GL applies each TWICE with an 8-bit round between — so Metal must too
 * or it diverges at non-default gamma / color-scale/bias. At the default gamma=1
 * (scale 1, bias 0) pass 1 is an identity 8-bit copy, so this stays byte-exact
 * with the prior single-pass. s_final_color is then blitted to the drawable AND
 * used as the readback source. Returns false if unavailable (caller blits raw). */
static bool mtl_run_output_filter(id<MTLCommandBuffer> cmdbuf, bool smaa_ran) {
    if (!mtl_ensure_filter_program() || s_scene_color == nil ||
        s_filter_low == nil || s_final_color == nil) return false;
    struct MtlFilterUniforms u;
    /* SMAA (W3.E4): when the 3-pass chain produced s_smaa_out this frame, the
     * pre-pass reads the anti-aliased scene instead of the raw scene (one array
     * entry). Byte-identical when SMAA is off (smaa_ran == false). */
    id<MTLTexture> pass0_src = (smaa_ran && s_smaa_out != nil) ? s_smaa_out : s_scene_color;
    id<MTLTexture> passes_src[2] = { pass0_src, s_filter_low };
    id<MTLTexture> passes_dst[2] = { s_filter_low, s_final_color };
    for (int pass = 0; pass < 2; pass++) {
        MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = passes_dst[pass];
        rpd.colorAttachments[0].loadAction = MTLLoadActionDontCare;  /* triangle covers all */
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (mtl_gpu_trace_on()) mtl_gpu_ts_attach_post(rpd);  /* W3.E1: post span */
        id<MTLRenderCommandEncoder> enc = [cmdbuf renderCommandEncoderWithDescriptor:rpd];
        [enc setRenderPipelineState:s_filter_pso];
        mtl_fill_filter_uniforms(&u, pass /* 0 = pre-pass, 1 = final */);
        [enc setFragmentBytes:&u length:sizeof u atIndex:1];
        [enc setFragmentTexture:passes_src[pass] atIndex:0];
        [enc setFragmentSamplerState:s_filter_smp atIndex:0];
        /* Scene depth for SSAO (final pass samples it when u.ssao==1; bound in
         * both passes so the declared depth2d arg is always satisfied). */
        [enc setFragmentTexture:s_scene_depth atIndex:1];
        [enc setFragmentSamplerState:s_depth_sampler atIndex:1];
        /* SSAO v2 precomputed AO (texture 2). Bound always so the declared arg is
         * satisfied even for planar/off (where the shader never reads it); the
         * linear s_filter_smp upsamples half-res AO in the composite. Fall back to
         * the 1x1 white tex when no AO buffer exists. */
        [enc setFragmentTexture:(s_ssao_raw != nil ? s_ssao_raw : s_white_tex) atIndex:2];
        [enc setFragmentSamplerState:s_filter_smp atIndex:2];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }
    return true;
}

/* ==========================================================================
 * SSAO v2 (W3.E2) — hemisphere AO with view-space reconstruction (Metal only).
 * The per-pixel viewPos()/viewNormal() reconstruction is the exact op that hangs
 * Apple's GL-over-Metal translator (docs/design/METAL_BACKEND_PLAN.md:39); it runs clean
 * natively. PASS A integrates a cosine hemisphere above the reconstructed surface;
 * PASS B (T3) blurs it bilaterally; the composite (mtl_ensure_filter_program)
 * samples the result as texture(2). All AO work is inserted in mtl_end_frame
 * between the scene-encoder close and the output filter.
 * ========================================================================== */

/* (Re)allocate the R8Unorm AO targets at w>>hr,h>>hr (half-res aware). Keyed on
 * (aoW,aoH,hr) so a live Video.SsaoHalfRes toggle or window resize re-sizes them
 * without touching the scene-target early-out in mtl_ensure_targets. */
static void mtl_ensure_ssao_targets(void) {
    int hr = g_pcSsaoHalfRes ? 1 : 0;
    int aw = s_fb_w >> hr, ah = s_fb_h >> hr;
    if (aw < 1) aw = 1;
    if (ah < 1) ah = 1;
    if (s_ssao_raw != nil && s_ssao_w == aw && s_ssao_h == ah && s_ssao_hr == hr) return;
    MTLTextureDescriptor *rd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                           width:aw height:ah mipmapped:NO];
    rd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    rd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> raw = [s_device newTextureWithDescriptor:rd];
    id<MTLTexture> tmp = [s_device newTextureWithDescriptor:rd];
    if (raw == nil || tmp == nil) { s_ssao_raw = nil; s_ssao_tmp = nil; return; }
    s_ssao_raw = raw;
    s_ssao_tmp = tmp;
    s_ssao_w = aw;
    s_ssao_h = ah;
    s_ssao_hr = hr;
}

/* Build the SSAO program: a shared fullscreen-triangle VS + PASS A hemisphere
 * fragment. Structured like mtl_ensure_filter_program. kKernel is 12 fixed
 * cosine-weighted hemisphere dirs generated ONCE offline (seed 7, scale_i =
 * mix(0.1,1.0,(i/12)^2)) — first-party math, no runtime RNG. The PASS B blur
 * fragment/PSO are appended by W3.E2.T3. */
static bool mtl_ensure_ssao_program(void) {
    if (s_ssao_pso != nil) return true;
    if (s_device == nil) return false;
    TB s; tb_init(&s, 16384);
    tb_str(&s, "#include <metal_stdlib>\nusing namespace metal;\n");
    tb_str(&s, "struct SsaoUniforms {\n");
    tb_str(&s, "  float projA, projB, projX, projY, radius, bias, power, farCutoff, nearCut, skyCut, blurDepthSharp, aoSizeW, aoSizeH;\n");
    tb_str(&s, "  int frameParity, blurAxis, _pad;\n");
    tb_str(&s, "};\n");
    tb_str(&s, "struct FVO { float4 position [[position]]; float2 vTexCoord; };\n");
    /* Same fullscreen triangle + vTexCoord mapping as filterVS so PASS A writes
     * and the composite reads at identical normalized-framebuffer coordinates. */
    tb_str(&s, "vertex FVO ssaoVS(uint vid [[vertex_id]]) {\n");
    tb_str(&s, "  const float2 kPos[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n");
    tb_str(&s, "  FVO o; o.position = float4(kPos[vid], 0.0, 1.0);\n");
    tb_str(&s, "  o.vTexCoord = float2(kPos[vid].x * 0.5 + 0.5, 0.5 - kPos[vid].y * 0.5); return o;\n}\n");
    /* View-space position from UV + window depth. z = GL's linearization (positive
     * view distance); the frontend z=(z+w)/2 remap makes Metal window depth == GL's
     * (proven for v1). This division is THE op that hangs GL-over-Metal. */
    tb_str(&s, "static float3 viewPos(float2 uv, float d, constant SsaoUniforms& u) {\n");
    tb_str(&s, "  float z = u.projB / (u.projA + 2.0 * d - 1.0);\n");
    tb_str(&s, "  float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n");
    tb_str(&s, "  return float3(ndc.x * z / u.projX, ndc.y * z / u.projY, z);\n}\n");
    /* Face normal from depth: least-slope neighbor pair kills the 1px edge ring a
     * naive dfdx/dfdy cross produces at depth discontinuities. */
    tb_str(&s, "static float3 viewNormal(depth2d<float> dep, sampler ds, float2 uv, float2 px, float3 P, constant SsaoUniforms& u) {\n");
    tb_str(&s, "  float3 Pr = viewPos(uv + float2(px.x, 0.0), dep.sample(ds, uv + float2(px.x, 0.0)), u);\n");
    tb_str(&s, "  float3 Pl = viewPos(uv - float2(px.x, 0.0), dep.sample(ds, uv - float2(px.x, 0.0)), u);\n");
    tb_str(&s, "  float3 Pd = viewPos(uv + float2(0.0, px.y), dep.sample(ds, uv + float2(0.0, px.y)), u);\n");
    tb_str(&s, "  float3 Pu = viewPos(uv - float2(0.0, px.y), dep.sample(ds, uv - float2(0.0, px.y)), u);\n");
    tb_str(&s, "  float3 dx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);\n");
    tb_str(&s, "  float3 dy = (abs(Pd.z - P.z) < abs(P.z - Pu.z)) ? (Pd - P) : (P - Pu);\n");
    tb_str(&s, "  return normalize(cross(dy, dx));\n}\n");
    /* Interleaved gradient noise — per-pixel kernel rotation, no noise texture. */
    tb_str(&s, "static float ign(float2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }\n");
    tb_str(&s, "constant float3 kKernel[12] = {\n");
    tb_str(&s, "    float3(-0.090424, 0.007589, 0.042023),\n");
    tb_str(&s, "    float3(-0.105740, 0.001779, 0.010242),\n");
    tb_str(&s, "    float3(-0.018828, -0.122054, 0.019320),\n");
    tb_str(&s, "    float3(-0.034110, 0.147704, 0.037872),\n");
    tb_str(&s, "    float3(0.081256, -0.074640, 0.166812),\n");
    tb_str(&s, "    float3(0.177093, -0.071080, 0.171026),\n");
    tb_str(&s, "    float3(0.143537, -0.078537, 0.280810),\n");
    tb_str(&s, "    float3(0.312853, 0.211482, 0.149791),\n");
    tb_str(&s, "    float3(0.083129, 0.028144, 0.492237),\n");
    tb_str(&s, "    float3(-0.601245, -0.019068, 0.075366),\n");
    tb_str(&s, "    float3(0.282314, 0.444025, 0.498764),\n");
    tb_str(&s, "    float3(0.297633, 0.251981, 0.762289),\n");
    tb_str(&s, "};\n");
    tb_str(&s, "fragment float4 ssaoFragment(FVO in [[stage_in]], constant SsaoUniforms& u [[buffer(1)]], depth2d<float> dep [[texture(0)]], sampler ds [[sampler(0)]]) {\n");
    tb_str(&s, "  float2 uv = in.vTexCoord;\n");
    tb_str(&s, "  float d = dep.sample(ds, uv);\n");
    tb_str(&s, "  if (d <= u.nearCut || d >= u.skyCut) return float4(1.0);\n");   /* viewmodel / sky */
    tb_str(&s, "  float3 P = viewPos(uv, d, u);\n");
    tb_str(&s, "  if (P.z >= u.farCutoff) return float4(1.0);\n");                /* noisy-depth zone */
    tb_str(&s, "  float2 px = 1.0 / float2(u.aoSizeW, u.aoSizeH);\n");
    tb_str(&s, "  float3 N = viewNormal(dep, ds, uv, px, P, u);\n");
    tb_str(&s, "  float a = ign(in.position.xy) * 6.2831853;\n");
    tb_str(&s, "  float3 randv = float3(cos(a), sin(a), 0.0);\n");
    tb_str(&s, "  float3 T = normalize(randv - N * dot(randv, N));\n");
    tb_str(&s, "  float3x3 TBN = float3x3(T, cross(N, T), N);\n");
    tb_str(&s, "  float fade = 1.0 - smoothstep(0.75 * u.farCutoff, u.farCutoff, P.z);\n");
    /* Occlusion by view-space reconstruction (grazing-robust, targets §7 risk 1):
     * for each hemisphere direction, reproject P+dir*radius to screen, reconstruct
     * the ACTUAL geometry point S there, and count it only if S rises above P's
     * tangent plane (dot(N, S-P) > bias) — coplanar grazing-ground neighbours have
     * dot ~= 0 and are rejected, so a flat plane self-occludes ~0 regardless of
     * view angle, while genuine occluders poking above the surface still register.
     * rangeChk rejects occluders beyond the radius (and the sky, which is far). */
    tb_str(&s, "  float occ = 0.0;\n");
    tb_str(&s, "  for (int i = 0; i < 12; ++i) {\n");
    tb_str(&s, "    float3 sp = P + (TBN * kKernel[i]) * u.radius;\n");
    /* Guard the perspective divide: a hemisphere sample can land at/behind the camera
     * plane (sp.z <= 0) for geometry close to the near cut, where sp.z/proj blows up and
     * the sign flip reprojects it to the opposite screen edge — sampling garbage depth
     * that produces flickering AO speckle on near surfaces. Reject samples behind the
     * camera or off-screen (standard hemisphere-SSAO practice); a rejected sample counts
     * as unoccluded, which is correct for something outside the reconstructable frame. */
    tb_str(&s, "    if (sp.z <= 0.0) continue;\n");
    tb_str(&s, "    float2 suv = float2(sp.x * u.projX / sp.z, sp.y * u.projY / sp.z);\n");
    tb_str(&s, "    suv = float2(suv.x * 0.5 + 0.5, 0.5 - suv.y * 0.5);\n");
    tb_str(&s, "    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;\n");
    tb_str(&s, "    float3 S = viewPos(suv, dep.sample(ds, suv), u);\n");
    tb_str(&s, "    float3 v = S - P; float len = max(length(v), 1e-4);\n");
    tb_str(&s, "    float ndl = dot(N, v / len);\n");
    tb_str(&s, "    float rangeChk = smoothstep(0.0, 1.0, u.radius / len);\n");
    tb_str(&s, "    occ += (ndl > u.bias) ? rangeChk : 0.0;\n");
    tb_str(&s, "  }\n");
    tb_str(&s, "  return float4(pow(saturate(1.0 - occ / 12.0), u.power) * fade + (1.0 - fade));\n}\n");

    /* PASS B (W3.E2.T3): separable 9-tap depth-weighted (bilateral) blur. Weight
     * w_i = g_i * exp(-blurDepthSharp * |linZc - linZi| / linZc) preserves AO edges
     * across depth discontinuities while smoothing the interleaved-gradient noise
     * and half-res coarseness. blurAxis picks X (0) then Y (1); runs at AO res. */
    tb_str(&s, "constant float kBlurG[5] = { 0.227027, 0.194594, 0.121622, 0.054054, 0.016216 };\n");
    tb_str(&s, "fragment float4 ssaoBlurFragment(FVO in [[stage_in]], constant SsaoUniforms& u [[buffer(1)]], texture2d<float> aoTex [[texture(0)]], sampler aoSmp [[sampler(0)]], depth2d<float> dep [[texture(1)]], sampler ds [[sampler(1)]]) {\n");
    tb_str(&s, "  float2 uv = in.vTexCoord;\n");
    tb_str(&s, "  float2 dir = (u.blurAxis == 0) ? float2(1.0 / u.aoSizeW, 0.0) : float2(0.0, 1.0 / u.aoSizeH);\n");
    tb_str(&s, "  float linZc = u.projB / (u.projA + 2.0 * dep.sample(ds, uv) - 1.0);\n");
    tb_str(&s, "  float sum = aoTex.sample(aoSmp, uv).r * kBlurG[0]; float wsum = kBlurG[0];\n");
    tb_str(&s, "  for (int i = 1; i <= 4; ++i) {\n");
    tb_str(&s, "    for (int sgn = -1; sgn <= 1; sgn += 2) {\n");
    tb_str(&s, "      float2 suv = uv + dir * (float(i) * float(sgn));\n");
    tb_str(&s, "      float linZi = u.projB / (u.projA + 2.0 * dep.sample(ds, suv) - 1.0);\n");
    tb_str(&s, "      float wz = exp(-u.blurDepthSharp * abs(linZc - linZi) / max(linZc, 1e-4));\n");
    tb_str(&s, "      float w = kBlurG[i] * wz; sum += aoTex.sample(aoSmp, suv).r * w; wsum += w;\n");
    tb_str(&s, "    }\n  }\n");
    tb_str(&s, "  return float4(sum / max(wsum, 1e-4));\n}\n");

  @autoreleasepool {  /* MSL compile transients (M3.2) */
    NSError *err = nil;
    NSString *nssrc = [NSString stringWithUTF8String:s.p];
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    if (@available(macOS 15.0, *)) { opts.mathMode = MTLMathModeSafe; }
#endif
    s_ssao_lib = [s_device newLibraryWithSource:nssrc options:opts error:&err];
    tb_free(&s);
    if (s_ssao_lib == nil) {
        fprintf(stderr, "[metal] SSAO MSL compile failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
        return false;
    }
    id<MTLFunction> vfn = [s_ssao_lib newFunctionWithName:@"ssaoVS"];
    id<MTLFunction> ffn = [s_ssao_lib newFunctionWithName:@"ssaoFragment"];
    if (vfn == nil || ffn == nil) return false;
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vfn;
    pd.fragmentFunction = ffn;
    pd.rasterSampleCount = 1;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
    pd.colorAttachments[0].blendingEnabled = NO;
    s_ssao_pso = [s_device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (s_ssao_pso == nil) {
        fprintf(stderr, "[metal] SSAO PSO build failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
        return false;
    }
    id<MTLFunction> bfn = [s_ssao_lib newFunctionWithName:@"ssaoBlurFragment"];
    if (bfn != nil) {
        MTLRenderPipelineDescriptor *bp = [[MTLRenderPipelineDescriptor alloc] init];
        bp.vertexFunction = vfn;
        bp.fragmentFunction = bfn;
        bp.rasterSampleCount = 1;
        bp.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
        bp.colorAttachments[0].blendingEnabled = NO;
        s_ssao_blur_pso = [s_device newRenderPipelineStateWithDescriptor:bp error:&err];
        if (s_ssao_blur_pso == nil) {
            fprintf(stderr, "[metal] SSAO blur PSO build failed: %s\n",
                    err ? err.localizedDescription.UTF8String : "(no error)");
        }
    }
    return true;
  }
}

static void mtl_fill_ssao_uniforms(struct MtlSsaoUniforms *u) {
    memset(u, 0, sizeof *u);
    u->projA = g_pc_ssao_proj_a;
    u->projB = g_pc_ssao_proj_b;
    u->projX = g_pc_ssao_proj_x;
    u->projY = g_pc_ssao_proj_y;
    u->radius = g_pcSsaoRadius * 80.0f;  /* kRadiusWorldScale: 0.5 -> 40 GE view-units (crate-scale reach) */
    u->bias = g_pcSsaoBias;
    u->power = g_pcSsaoPower;
    u->farCutoff = g_pcSsaoFarCutoff;
    u->nearCut = g_pcSsaoNearCut;
    u->skyCut = g_pcSsaoSkyCut;
    u->blurDepthSharp = g_pcSsaoBlurDepthSharp;
    u->aoSizeW = (float)s_ssao_w;
    u->aoSizeH = (float)s_ssao_h;
    u->frameParity = 0;
    u->blurAxis = 0;
}

/* One separable-blur pass: read `src` R8 AO, write `dst`, along axis (0=X,1=Y).
 * Depth-weighted; automatic hazard tracking orders it behind the prior write. */
static void mtl_ssao_blur_pass(id<MTLCommandBuffer> cmdbuf, struct MtlSsaoUniforms *u,
                               id<MTLTexture> src, id<MTLTexture> dst, int axis) {
    u->blurAxis = axis;
    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = dst;
    rpd.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (mtl_gpu_trace_on()) mtl_gpu_ts_attach_post(rpd);  /* W3.E1: post span */
    id<MTLRenderCommandEncoder> enc = [cmdbuf renderCommandEncoderWithDescriptor:rpd];
    [enc setRenderPipelineState:s_ssao_blur_pso];
    [enc setFragmentBytes:u length:sizeof *u atIndex:1];
    [enc setFragmentTexture:src atIndex:0];
    [enc setFragmentSamplerState:s_filter_smp atIndex:0];    /* AO taps (linear) */
    [enc setFragmentTexture:s_scene_depth atIndex:1];
    [enc setFragmentSamplerState:s_depth_sampler atIndex:1]; /* depth (nearest) */
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

/* Run the AO chain into s_ssao_raw. PASS A always; PASS B (bilateral blur,
 * X then Y ping-pong through s_ssao_tmp) when Video.SsaoBlur. Automatic Metal
 * hazard tracking serializes the composite's texture(2) read behind these writes
 * (same command buffer, tracked private textures). */
static void mtl_run_ssao(id<MTLCommandBuffer> cmdbuf) {
    if (!mtl_ensure_ssao_program()) return;
    mtl_ensure_ssao_targets();
    if (s_ssao_raw == nil || s_scene_depth == nil) return;
    struct MtlSsaoUniforms u;
    mtl_fill_ssao_uniforms(&u);
    /* PASS A: hemisphere AO -> s_ssao_raw */
    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = s_ssao_raw;
    rpd.colorAttachments[0].loadAction = MTLLoadActionDontCare;   /* triangle covers all */
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (mtl_gpu_trace_on()) mtl_gpu_ts_attach_post(rpd);  /* W3.E1: post span */
    id<MTLRenderCommandEncoder> enc = [cmdbuf renderCommandEncoderWithDescriptor:rpd];
    [enc setRenderPipelineState:s_ssao_pso];
    [enc setFragmentBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentTexture:s_scene_depth atIndex:0];
    [enc setFragmentSamplerState:s_depth_sampler atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    /* PASS B: separable bilateral blur, ping-pong s_ssao_raw -> s_ssao_tmp -> s_ssao_raw */
    if (g_pcSsaoBlur && s_ssao_blur_pso != nil && s_ssao_tmp != nil) {
        mtl_ssao_blur_pass(cmdbuf, &u, s_ssao_raw, s_ssao_tmp, 0);
        mtl_ssao_blur_pass(cmdbuf, &u, s_ssao_tmp, s_ssao_raw, 1);
    }
}

/* ==========================================================================
 * SMAA 1x (W3.E4) — subpixel morphological AA (Metal only). Three spatial
 * passes run in mtl_end_frame after the scene encoder closes and BEFORE the
 * output filter: (1) luma edge detection -> s_smaa_edges (RG8); (2) blend-weight
 * calculation using the committed AreaTex/SearchTex LUTs -> s_smaa_blend (RGBA8);
 * (3) neighborhood blending of the scene color -> s_smaa_out (BGRA8). The output
 * filter then reads s_smaa_out as passes_src[0] and forces FXAA off. Deterministic
 * (pure spatial, no history), so it stays screenshot-gate friendly. MSL is a
 * faithful port of the SMAA reference D3D path (Jimenez et al., MIT), 1x profile:
 * no diagonal/corner/predication/temporal (subsampleIndices = 0). Metal's
 * top-left texel origin matches the reference's D3D convention, and the LUTs are
 * uploaded verbatim (row 0 first) exactly as the reference D3D headers expect. */
#define MTL_SMAA_SEARCH_STEPS 16   /* SMAA "high" — search reach along an edge (budget dial) */

struct MtlSmaaUniforms { float rtMetrics[4]; };  /* (1/w, 1/h, w, h); 16 bytes */

static id<MTLTexture> s_smaa_area_gpu = nil;
static id<MTLTexture> s_smaa_search_gpu = nil;
static id<MTLLibrary> s_smaa_lib = nil;
static id<MTLRenderPipelineState> s_smaa_edge_pso = nil;
static id<MTLRenderPipelineState> s_smaa_blend_pso = nil;
static id<MTLRenderPipelineState> s_smaa_neighbor_pso = nil;
static id<MTLSamplerState> s_smaa_linear_smp = nil;   /* linear clamp (edges/area/search/color) */
static id<MTLSamplerState> s_smaa_point_smp = nil;    /* nearest clamp (edge-detect color taps) */

/* Scene-sized SMAA targets; lazily (re)allocated on size change, decoupled from
 * mtl_ensure_targets so an alloc failure only disables SMAA (leaves the scene
 * path intact). Returns false if the targets are not usable this frame. */
static bool mtl_ensure_smaa_targets(void) {
    if (s_fb_w <= 0 || s_fb_h <= 0) return false;
    if (s_smaa_out != nil && s_smaa_w == s_fb_w && s_smaa_h == s_fb_h) return true;
    MTLTextureDescriptor *ed =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm
                                                           width:s_fb_w height:s_fb_h mipmapped:NO];
    ed.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    ed.storageMode = MTLStorageModePrivate;
    id<MTLTexture> edges = [s_device newTextureWithDescriptor:ed];
    MTLTextureDescriptor *bd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:s_fb_w height:s_fb_h mipmapped:NO];
    bd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    bd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> blend = [s_device newTextureWithDescriptor:bd];
    MTLTextureDescriptor *od =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:s_fb_w height:s_fb_h mipmapped:NO];
    od.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    od.storageMode = MTLStorageModePrivate;
    id<MTLTexture> out = [s_device newTextureWithDescriptor:od];
    if (edges == nil || blend == nil || out == nil) {
        s_smaa_edges = nil; s_smaa_blend = nil; s_smaa_out = nil;
        return false;
    }
    s_smaa_edges = edges; s_smaa_blend = blend; s_smaa_out = out;
    s_smaa_w = s_fb_w; s_smaa_h = s_fb_h;
    return true;
}

/* Upload the two committed LUTs once (shared storage, replaceRegion). */
static bool mtl_ensure_smaa_luts(void) {
    if (s_smaa_area_gpu != nil && s_smaa_search_gpu != nil) return true;
    MTLTextureDescriptor *ad =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm
                                                           width:SMAA_AREATEX_WIDTH
                                                          height:SMAA_AREATEX_HEIGHT mipmapped:NO];
    ad.usage = MTLTextureUsageShaderRead;
    ad.storageMode = MTLStorageModeShared;
    id<MTLTexture> area = [s_device newTextureWithDescriptor:ad];
    MTLTextureDescriptor *sd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                           width:SMAA_SEARCHTEX_WIDTH
                                                          height:SMAA_SEARCHTEX_HEIGHT mipmapped:NO];
    sd.usage = MTLTextureUsageShaderRead;
    sd.storageMode = MTLStorageModeShared;
    id<MTLTexture> search = [s_device newTextureWithDescriptor:sd];
    if (area == nil || search == nil) return false;
    [area replaceRegion:MTLRegionMake2D(0, 0, SMAA_AREATEX_WIDTH, SMAA_AREATEX_HEIGHT)
            mipmapLevel:0 withBytes:g_smaa_area_tex bytesPerRow:SMAA_AREATEX_PITCH];
    [search replaceRegion:MTLRegionMake2D(0, 0, SMAA_SEARCHTEX_WIDTH, SMAA_SEARCHTEX_HEIGHT)
              mipmapLevel:0 withBytes:g_smaa_search_tex bytesPerRow:SMAA_SEARCHTEX_PITCH];
    s_smaa_area_gpu = area;
    s_smaa_search_gpu = search;
    return true;
}

/* Build the SMAA library (shared fullscreen-triangle VS + 3 fragments) and the
 * three PSOs (RG8 / RGBA8 / BGRA8 targets) + two samplers. Returns false on any
 * failure (caller skips SMAA and the raw scene reaches the filter). */
static bool mtl_ensure_smaa_programs(void) {
    if (s_smaa_edge_pso != nil && s_smaa_blend_pso != nil && s_smaa_neighbor_pso != nil) return true;
    if (s_device == nil) return false;
    if (!mtl_ensure_smaa_luts()) return false;
    TB s; tb_init(&s, 16384);
    tb_str(&s, "#include <metal_stdlib>\nusing namespace metal;\n");
    tb_str(&s, "struct SmaaUniforms { float4 rtMetrics; };\n");
    tb_str(&s, "struct FVO { float4 position [[position]]; float2 vTexCoord; };\n");
    /* Same fullscreen triangle + top-left vTexCoord mapping as filterVS/ssaoVS. */
    tb_str(&s, "vertex FVO smaaVS(uint vid [[vertex_id]]) {\n");
    tb_str(&s, "  const float2 kPos[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n");
    tb_str(&s, "  FVO o; o.position = float4(kPos[vid], 0.0, 1.0);\n");
    tb_str(&s, "  o.vTexCoord = float2(kPos[vid].x * 0.5 + 0.5, 0.5 - kPos[vid].y * 0.5); return o;\n}\n");
    /* ---- PASS 1: luma edge detection (SMAALumaEdgeDetectionPS, threshold 0.1,
     * local-contrast-adaptation 2.0). Point-sampled color; returns (0,0) for
     * non-edges (every pixel written, so DontCare load is fine). ---- */
    tb_str(&s, "fragment float4 smaaEdgeFragment(FVO in [[stage_in]], constant SmaaUniforms& u [[buffer(1)]], texture2d<float> colorTex [[texture(0)]], sampler pnt [[sampler(0)]]) {\n");
    tb_str(&s, "  float4 rt = u.rtMetrics; float2 tc = in.vTexCoord;\n");
    tb_str(&s, "  float4 o0 = rt.xyxy * float4(-1.0, 0.0, 0.0, -1.0) + tc.xyxy;\n");
    tb_str(&s, "  float4 o1 = rt.xyxy * float4( 1.0, 0.0, 0.0,  1.0) + tc.xyxy;\n");
    tb_str(&s, "  float4 o2 = rt.xyxy * float4(-2.0, 0.0, 0.0, -2.0) + tc.xyxy;\n");
    tb_str(&s, "  float3 w = float3(0.2126, 0.7152, 0.0722);\n");
    tb_str(&s, "  float L      = dot(colorTex.sample(pnt, tc,     level(0)).rgb, w);\n");
    tb_str(&s, "  float Lleft  = dot(colorTex.sample(pnt, o0.xy,  level(0)).rgb, w);\n");
    tb_str(&s, "  float Ltop   = dot(colorTex.sample(pnt, o0.zw,  level(0)).rgb, w);\n");
    tb_str(&s, "  float4 delta; delta.xy = abs(L - float2(Lleft, Ltop));\n");
    tb_str(&s, "  float2 edges = step(float2(0.1), delta.xy);\n");
    tb_str(&s, "  if (dot(edges, float2(1.0)) == 0.0) return float4(0.0);\n");
    tb_str(&s, "  float Lright  = dot(colorTex.sample(pnt, o1.xy, level(0)).rgb, w);\n");
    tb_str(&s, "  float Lbottom = dot(colorTex.sample(pnt, o1.zw, level(0)).rgb, w);\n");
    tb_str(&s, "  delta.zw = abs(L - float2(Lright, Lbottom));\n");
    tb_str(&s, "  float2 maxDelta = max(delta.xy, delta.zw);\n");
    tb_str(&s, "  float Lll = dot(colorTex.sample(pnt, o2.xy, level(0)).rgb, w);\n");
    tb_str(&s, "  float Ltt = dot(colorTex.sample(pnt, o2.zw, level(0)).rgb, w);\n");
    tb_str(&s, "  delta.zw = abs(float2(Lleft, Ltop) - float2(Lll, Ltt));\n");
    tb_str(&s, "  maxDelta = max(maxDelta, delta.zw);\n");
    tb_str(&s, "  float finalDelta = max(maxDelta.x, maxDelta.y);\n");
    tb_str(&s, "  edges *= step(float2(finalDelta), 2.0 * delta.xy);\n");
    tb_str(&s, "  return float4(edges, 0.0, 0.0);\n}\n");
    /* ---- Search helpers for PASS 2 (linear-sampled edges exploit the diagonal
     * offsets' bilinear fetch). SEARCHTEX size 66x33 packed into 64x16. ---- */
    tb_str(&s, "static float smaaSearchLength(texture2d<float> searchTex, sampler lin, float2 e, float off) {\n");
    tb_str(&s, "  float2 SZ = float2(66.0, 33.0); float2 PZ = float2(64.0, 16.0);\n");
    tb_str(&s, "  float2 scale = SZ * float2(0.5, -1.0) + float2(-1.0, 1.0);\n");
    tb_str(&s, "  float2 bias  = SZ * float2(off, 1.0) + float2(0.5, -0.5);\n");
    tb_str(&s, "  scale /= PZ; bias /= PZ;\n");
    tb_str(&s, "  return searchTex.sample(lin, scale * e + bias, level(0)).r;\n}\n");
    tb_str(&s, "static float smaaSearchXLeft(texture2d<float> edgesTex, texture2d<float> searchTex, sampler lin, float2 tc, float end, float4 rt) {\n");
    tb_str(&s, "  float2 e = float2(0.0, 1.0);\n");
    tb_str(&s, "  for (int i = 0; i < 16; i++) { if (!(tc.x > end && e.g > 0.8281 && e.r == 0.0)) break;\n");
    tb_str(&s, "    e = edgesTex.sample(lin, tc, level(0)).rg; tc -= float2(2.0, 0.0) * rt.xy; }\n");
    tb_str(&s, "  float off = -(255.0/127.0) * smaaSearchLength(searchTex, lin, e, 0.0) + 3.25;\n");
    tb_str(&s, "  return rt.x * off + tc.x;\n}\n");
    tb_str(&s, "static float smaaSearchXRight(texture2d<float> edgesTex, texture2d<float> searchTex, sampler lin, float2 tc, float end, float4 rt) {\n");
    tb_str(&s, "  float2 e = float2(0.0, 1.0);\n");
    tb_str(&s, "  for (int i = 0; i < 16; i++) { if (!(tc.x < end && e.g > 0.8281 && e.r == 0.0)) break;\n");
    tb_str(&s, "    e = edgesTex.sample(lin, tc, level(0)).rg; tc += float2(2.0, 0.0) * rt.xy; }\n");
    tb_str(&s, "  float off = -(255.0/127.0) * smaaSearchLength(searchTex, lin, e, 0.5) + 3.25;\n");
    tb_str(&s, "  return -rt.x * off + tc.x;\n}\n");
    tb_str(&s, "static float smaaSearchYUp(texture2d<float> edgesTex, texture2d<float> searchTex, sampler lin, float2 tc, float end, float4 rt) {\n");
    tb_str(&s, "  float2 e = float2(1.0, 0.0);\n");
    tb_str(&s, "  for (int i = 0; i < 16; i++) { if (!(tc.y > end && e.r > 0.8281 && e.g == 0.0)) break;\n");
    tb_str(&s, "    e = edgesTex.sample(lin, tc, level(0)).rg; tc -= float2(0.0, 2.0) * rt.xy; }\n");
    tb_str(&s, "  float off = -(255.0/127.0) * smaaSearchLength(searchTex, lin, e.gr, 0.0) + 3.25;\n");
    tb_str(&s, "  return rt.y * off + tc.y;\n}\n");
    tb_str(&s, "static float smaaSearchYDown(texture2d<float> edgesTex, texture2d<float> searchTex, sampler lin, float2 tc, float end, float4 rt) {\n");
    tb_str(&s, "  float2 e = float2(1.0, 0.0);\n");
    tb_str(&s, "  for (int i = 0; i < 16; i++) { if (!(tc.y < end && e.r > 0.8281 && e.g == 0.0)) break;\n");
    tb_str(&s, "    e = edgesTex.sample(lin, tc, level(0)).rg; tc += float2(0.0, 2.0) * rt.xy; }\n");
    tb_str(&s, "  float off = -(255.0/127.0) * smaaSearchLength(searchTex, lin, e.gr, 0.5) + 3.25;\n");
    tb_str(&s, "  return -rt.y * off + tc.y;\n}\n");
    tb_str(&s, "static float2 smaaArea(texture2d<float> areaTex, sampler lin, float2 dist, float e1, float e2, float off) {\n");
    tb_str(&s, "  float2 PS = float2(1.0/160.0, 1.0/560.0);\n");
    tb_str(&s, "  float2 tc = 16.0 * round(4.0 * float2(e1, e2)) + dist;\n");
    tb_str(&s, "  tc = PS * tc + 0.5 * PS;\n");
    tb_str(&s, "  tc.y = (1.0/7.0) * off + tc.y;\n");
    tb_str(&s, "  return areaTex.sample(lin, tc, level(0)).rg;\n}\n");
    /* ---- PASS 2: blend-weight calculation (SMAABlendingWeightCalculationPS, 1x:
     * subsampleIndices = 0, no diag/corner). ---- */
    tb_str(&s, "fragment float4 smaaBlendFragment(FVO in [[stage_in]], constant SmaaUniforms& u [[buffer(1)]], texture2d<float> edgesTex [[texture(0)]], texture2d<float> areaTex [[texture(1)]], texture2d<float> searchTex [[texture(2)]], sampler lin [[sampler(0)]]) {\n");
    tb_str(&s, "  float4 rt = u.rtMetrics; float2 tc = in.vTexCoord; float2 pix = tc * rt.zw;\n");
    tb_str(&s, "  float4 o0 = rt.xyxy * float4(-0.25, -0.125,  1.25, -0.125) + tc.xyxy;\n");
    tb_str(&s, "  float4 o1 = rt.xyxy * float4(-0.125, -0.25, -0.125,  1.25) + tc.xyxy;\n");
    tb_str(&s, "  float4 o2 = rt.xxyy * (float4(-2.0, 2.0, -2.0, 2.0) * 16.0) + float4(o0.xz, o1.yw);\n");
    tb_str(&s, "  float4 weights = float4(0.0);\n");
    tb_str(&s, "  float2 e = edgesTex.sample(lin, tc, level(0)).rg;\n");
    tb_str(&s, "  if (e.g > 0.0) {\n");
    tb_str(&s, "    float2 d; float3 c;\n");
    tb_str(&s, "    c.x = smaaSearchXLeft(edgesTex, searchTex, lin, o0.xy, o2.x, rt); c.y = o1.y; d.x = c.x;\n");
    tb_str(&s, "    float e1 = edgesTex.sample(lin, c.xy, level(0)).r;\n");
    tb_str(&s, "    c.z = smaaSearchXRight(edgesTex, searchTex, lin, o0.zw, o2.y, rt); d.y = c.z;\n");
    tb_str(&s, "    d = abs(round(rt.zz * d - pix.xx)); float2 sq = sqrt(d);\n");
    tb_str(&s, "    float e2 = edgesTex.sample(lin, c.zy, level(0), int2(1, 0)).r;\n");
    tb_str(&s, "    weights.rg = smaaArea(areaTex, lin, sq, e1, e2, 0.0);\n  }\n");
    tb_str(&s, "  if (e.r > 0.0) {\n");
    tb_str(&s, "    float2 d; float3 c;\n");
    tb_str(&s, "    c.y = smaaSearchYUp(edgesTex, searchTex, lin, o1.xy, o2.z, rt); c.x = o0.x; d.x = c.y;\n");
    tb_str(&s, "    float e1 = edgesTex.sample(lin, c.xy, level(0)).g;\n");
    tb_str(&s, "    c.z = smaaSearchYDown(edgesTex, searchTex, lin, o1.zw, o2.w, rt); d.y = c.z;\n");
    tb_str(&s, "    d = abs(round(rt.ww * d - pix.yy)); float2 sq = sqrt(d);\n");
    tb_str(&s, "    float e2 = edgesTex.sample(lin, c.xz, level(0), int2(0, 1)).g;\n");
    tb_str(&s, "    weights.ba = smaaArea(areaTex, lin, sq, e1, e2, 0.0);\n  }\n");
    tb_str(&s, "  return weights;\n}\n");
    /* ---- PASS 3: neighborhood blending (SMAANeighborhoodBlendingPS). ---- */
    tb_str(&s, "fragment float4 smaaNeighborFragment(FVO in [[stage_in]], constant SmaaUniforms& u [[buffer(1)]], texture2d<float> colorTex [[texture(0)]], texture2d<float> blendTex [[texture(1)]], sampler lin [[sampler(0)]]) {\n");
    tb_str(&s, "  float4 rt = u.rtMetrics; float2 tc = in.vTexCoord;\n");
    tb_str(&s, "  float4 off = rt.xyxy * float4(1.0, 0.0, 0.0, 1.0) + tc.xyxy;\n");
    tb_str(&s, "  float4 a;\n");
    tb_str(&s, "  a.x = blendTex.sample(lin, off.xy, level(0)).a;\n");   /* right */
    tb_str(&s, "  a.y = blendTex.sample(lin, off.zw, level(0)).g;\n");   /* top */
    tb_str(&s, "  a.wz = blendTex.sample(lin, tc, level(0)).xz;\n");     /* bottom, left */
    tb_str(&s, "  if (dot(a, float4(1.0)) < 1e-5) return colorTex.sample(lin, tc, level(0));\n");
    tb_str(&s, "  bool h = max(a.x, a.z) > max(a.y, a.w);\n");
    tb_str(&s, "  float4 bOff = float4(0.0, a.y, 0.0, a.w); float2 bW = a.yw;\n");
    tb_str(&s, "  if (h) { bOff = float4(a.x, 0.0, a.z, 0.0); bW = a.xz; }\n");
    tb_str(&s, "  bW /= dot(bW, float2(1.0));\n");
    tb_str(&s, "  float4 bc = bOff * float4(rt.x, rt.y, -rt.x, -rt.y) + tc.xyxy;\n");
    tb_str(&s, "  float4 color = bW.x * colorTex.sample(lin, bc.xy, level(0));\n");
    tb_str(&s, "  color += bW.y * colorTex.sample(lin, bc.zw, level(0));\n");
    tb_str(&s, "  return color;\n}\n");

  @autoreleasepool {  /* MSL compile transients (M3.2) */
    NSError *err = nil;
    NSString *nssrc = [NSString stringWithUTF8String:s.p];
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
    s_smaa_lib = [s_device newLibraryWithSource:nssrc options:opts error:&err];
    tb_free(&s);
    if (s_smaa_lib == nil) {
        fprintf(stderr, "[metal] SMAA MSL compile failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
        return false;
    }
    id<MTLFunction> vfn = [s_smaa_lib newFunctionWithName:@"smaaVS"];
    id<MTLFunction> efn = [s_smaa_lib newFunctionWithName:@"smaaEdgeFragment"];
    id<MTLFunction> bfn = [s_smaa_lib newFunctionWithName:@"smaaBlendFragment"];
    id<MTLFunction> nfn = [s_smaa_lib newFunctionWithName:@"smaaNeighborFragment"];
    if (vfn == nil || efn == nil || bfn == nil || nfn == nil) return false;
    struct { id<MTLFunction> f; MTLPixelFormat fmt; __strong id<MTLRenderPipelineState> *dst; } pk[3] = {
        { efn, MTLPixelFormatRG8Unorm,   &s_smaa_edge_pso },
        { bfn, MTLPixelFormatRGBA8Unorm, &s_smaa_blend_pso },
        { nfn, MTLPixelFormatBGRA8Unorm, &s_smaa_neighbor_pso },
    };
    for (int i = 0; i < 3; i++) {
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = vfn;
        pd.fragmentFunction = pk[i].f;
        pd.vertexDescriptor = nil;
        pd.rasterSampleCount = 1;
        pd.colorAttachments[0].pixelFormat = pk[i].fmt;
        pd.colorAttachments[0].blendingEnabled = NO;
        *pk[i].dst = [s_device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (*pk[i].dst == nil) {
            fprintf(stderr, "[metal] SMAA PSO %d build failed: %s\n", i,
                    err ? err.localizedDescription.UTF8String : "(no error)");
            return false;
        }
    }
    MTLSamplerDescriptor *ls = [[MTLSamplerDescriptor alloc] init];
    ls.minFilter = MTLSamplerMinMagFilterLinear;
    ls.magFilter = MTLSamplerMinMagFilterLinear;
    ls.mipFilter = MTLSamplerMipFilterNotMipmapped;
    ls.sAddressMode = MTLSamplerAddressModeClampToEdge;
    ls.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_smaa_linear_smp = [s_device newSamplerStateWithDescriptor:ls];
    MTLSamplerDescriptor *ps = [[MTLSamplerDescriptor alloc] init];
    ps.minFilter = MTLSamplerMinMagFilterNearest;
    ps.magFilter = MTLSamplerMinMagFilterNearest;
    ps.mipFilter = MTLSamplerMipFilterNotMipmapped;
    ps.sAddressMode = MTLSamplerAddressModeClampToEdge;
    ps.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_smaa_point_smp = [s_device newSamplerStateWithDescriptor:ps];
    return s_smaa_edge_pso != nil && s_smaa_blend_pso != nil && s_smaa_neighbor_pso != nil &&
           s_smaa_linear_smp != nil && s_smaa_point_smp != nil;
  }
}

/* Run the 3-pass SMAA chain on s_scene_color into s_smaa_out. Automatic Metal
 * hazard tracking serializes each pass behind the prior (same command buffer,
 * tracked private targets), and the output filter's read of s_smaa_out behind
 * PASS 3. Returns true iff s_smaa_out holds a valid anti-aliased scene. */
static bool mtl_run_smaa(id<MTLCommandBuffer> cmdbuf) {
    if (!mtl_ensure_smaa_programs()) return false;
    if (!mtl_ensure_smaa_targets()) return false;
    if (s_scene_color == nil) return false;
    struct MtlSmaaUniforms u;
    u.rtMetrics[0] = 1.0f / (float)s_fb_w;
    u.rtMetrics[1] = 1.0f / (float)s_fb_h;
    u.rtMetrics[2] = (float)s_fb_w;
    u.rtMetrics[3] = (float)s_fb_h;
    struct { id<MTLTexture> dst; id<MTLRenderPipelineState> pso; int nt; id<MTLTexture> t[3]; id<MTLSamplerState> smp; } passes[3] = {
        { s_smaa_edges,    s_smaa_edge_pso,     1, { s_scene_color, nil, nil },                        s_smaa_point_smp  },
        { s_smaa_blend,    s_smaa_blend_pso,    3, { s_smaa_edges, s_smaa_area_gpu, s_smaa_search_gpu }, s_smaa_linear_smp },
        { s_smaa_out,      s_smaa_neighbor_pso, 2, { s_scene_color, s_smaa_blend, nil },                s_smaa_linear_smp },
    };
    for (int p = 0; p < 3; p++) {
        MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = passes[p].dst;
        rpd.colorAttachments[0].loadAction = MTLLoadActionDontCare;   /* triangle covers all */
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (mtl_gpu_trace_on()) mtl_gpu_ts_attach_post(rpd);  /* W3.E1: post span */
        id<MTLRenderCommandEncoder> enc = [cmdbuf renderCommandEncoderWithDescriptor:rpd];
        [enc setRenderPipelineState:passes[p].pso];
        [enc setFragmentBytes:&u length:sizeof u atIndex:1];
        for (int t = 0; t < passes[p].nt; t++) [enc setFragmentTexture:passes[p].t[t] atIndex:t];
        [enc setFragmentSamplerState:passes[p].smp atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }
    return true;
}

static void mtl_end_frame(void) {
  @autoreleasepool {
    if (s_enc != nil) {
        [s_enc endEncoding];
        s_enc = nil;
    }
    if (s_cmdbuf == nil) return;
    const bool perf_traced = mtl_gpu_trace_on();  /* W3.E1: gate signposts+timers */
    /* Signal the frame throttle when THIS command buffer completes, and surface
     * GPU faults/device loss (otherwise silently swallowed). Balanced with the
     * start_frame wait (both gated on s_cmdbuf existing). If the readback path
     * split the frame, this is the final command buffer, and in-order completion
     * means all of the frame's buffers are done when it fires. */
    if (s_frame_sem != nil) {
        [s_cmdbuf addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            if (cb.status == MTLCommandBufferStatusError) {
                fprintf(stderr, "[metal] command buffer error: %s\n",
                        cb.error ? cb.error.localizedDescription.UTF8String : "(unknown)");
            }
            dispatch_semaphore_signal(s_frame_sem);
        }];
    }
    /* CAMetalLayer can resize drawableSize async during a window resize or a drag
     * across displays of differing backing scale. Re-assert the render size so
     * the equal-size check below doesn't drop the present and freeze the screen. */
    if ((int)s_layer.drawableSize.width != s_fb_w || (int)s_layer.drawableSize.height != s_fb_h) {
        s_layer.drawableSize = CGSizeMake(s_fb_w, s_fb_h);
    }
    /* W3.E1: the scene encoder is closed; everything below is "post". Close the
     * scene signpost and open the post one (Instruments Points of Interest). */
    os_signpost_id_t sp_post = 0;
    if (perf_traced) {
        os_signpost_interval_end(mtl_signpost_log(), s_sp_scene, "scene");
        sp_post = os_signpost_id_generate(mtl_signpost_log());
        os_signpost_interval_begin(mtl_signpost_log(), sp_post, "post");
    }
    /* SSAO v2 (W3.E2): PASS A/B into s_ssao_raw, between the scene-encoder close
     * and the output filter that samples it. Gated exactly like the composite
     * (RemasterFX && Ssao && hemisphere && proj_b captured this frame). */
    if (g_pcRemasterFX && g_pcSsao != 0 && g_pcSsaoMode == 2 && g_pc_ssao_proj_b != 0.0f &&
        !mtl_msaa_active()) {   /* FID-0018: no AO under MSAA (stale MS depth) — GL contract */
        mtl_run_ssao(s_cmdbuf);
    }
    /* SMAA (W3.E4): the 3-pass chain runs between the scene-encoder close and the
     * output filter, producing an anti-aliased s_smaa_out that the filter reads as
     * passes_src[0]. Gated on RemasterFX like FXAA; default Smaa=0 -> never runs
     * (byte-identical). smaa_ran carries into the filter so the source swap only
     * happens when the chain actually produced s_smaa_out. */
    bool smaa_ran = false;
    if (g_pcRemasterFX && g_pcSmaa != 0) {
        smaa_ran = mtl_run_smaa(s_cmdbuf);
    }
    /* Output-VI-filter chain (Phase 4) when any post-FX is active; otherwise the
     * raw scene — which keeps the faithful/default path byte-identical (and
     * covers the case where the filter program failed to build). The presented
     * texture is also the readback source so screenshots match the display. */
    id<MTLTexture> present_src = s_scene_color;
    if (mtl_output_filter_active() && mtl_run_output_filter(s_cmdbuf, smaa_ran)) {
        present_src = s_final_color;
    }
    if (present_src != nil) {
        s_minimap_overlay_target = present_src;
        minimap_overlay_draw_queued_frames_metal(s_fb_w, s_fb_h);
        s_minimap_overlay_target = nil;
    }
    s_readback_src = present_src;
    s_drawable = [s_layer nextDrawable];
    if (s_drawable != nil && present_src != nil &&
        (int)s_drawable.texture.width == s_fb_w && (int)s_drawable.texture.height == s_fb_h) {
        id<MTLBlitCommandEncoder> b = [s_cmdbuf blitCommandEncoder];
        [b copyFromTexture:present_src sourceSlice:0 sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(s_fb_w, s_fb_h, 1)
                 toTexture:s_drawable.texture destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
        [s_cmdbuf presentDrawable:s_drawable];
    }
    /* W3.E1: emit the per-frame GPU timing once this cmdbuf completes. Added here
     * (after the post passes attached their timestamps) so the captured slot
     * window is final; captured BY VALUE so the next frame's start_frame reset
     * can't race the async handler (the ring-slot counter region does the same
     * for the sample buffer contents). */
    if (perf_traced && s_cmdbuf != nil) {
        uint32_t perf_frame = s_frame_count_metal;
        int      perf_ts_frame_start = s_frame_ts_written ? s_frame_ts_slot : -1;
        int      perf_ts_first = s_post_ts_first;
        int      perf_ts_last  = s_post_ts_last;
        [s_cmdbuf addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            /* total = whole-frame GPU time (seconds domain). post = the directly
             * measured post-pass span; scene = the remainder. The post<=total
             * clamp is the physical bound (post is a subset of the frame) and
             * keeps scene>=0 and scene+post==total. */
            double total_ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
            if (total_ms < 0.0) total_ms = 0.0;
            double post_ms = 0.0;
            if (s_gpu_ts_ready && s_gpu_ts_buf != nil &&
                perf_ts_first >= 0 && perf_ts_last >= perf_ts_first) {
                NSUInteger n = (NSUInteger)perf_ts_last + 1;
                NSData *data = [s_gpu_ts_buf resolveCounterRange:NSMakeRange(0, n)];
                if (data != nil && data.length >= n * sizeof(MTLCounterResultTimestamp)) {
                    const MTLCounterResultTimestamp *ts =
                        (const MTLCounterResultTimestamp *)data.bytes;
                    uint64_t tp0 = ts[perf_ts_first].timestamp;   /* post-begin */
                    uint64_t tp1 = ts[perf_ts_last].timestamp;    /* post-end   */
                    /* Self-calibrate the counter tick unit per frame: the whole
                     * GPU frame span in ticks (scene-start -> post-end) maps to
                     * total_ms (GPUStart/End, authoritative seconds domain). This
                     * is device/unit-agnostic (Apple counter timestamps are not in
                     * the sampleTimestamps GPU domain). */
                    bool have_frame = (perf_ts_frame_start >= 0);
                    uint64_t tf0 = have_frame ? ts[perf_ts_frame_start].timestamp : 0;
                    if (have_frame && tf0 == MTLCounterErrorValue) have_frame = false;
                    if (have_frame && tp0 != MTLCounterErrorValue && tp1 != MTLCounterErrorValue &&
                        tp1 >= tp0 && tp1 > tf0 && total_ms > 0.0) {
                        double tick_ms = total_ms / (double)(tp1 - tf0);  /* ms per tick */
                        post_ms = (double)(tp1 - tp0) * tick_ms;
                    }
                }
            }
            if (post_ms > total_ms) post_ms = total_ms;
            double scene_ms = total_ms - post_ms;
            fprintf(stderr,
                    "[PERF-GPU] frame=%u total_ms=%.3f scene_ms=%.3f post_ms=%.3f\n",
                    perf_frame, total_ms, scene_ms, post_ms);
        }];
    }
    [s_cmdbuf commit];
    if (perf_traced) os_signpost_interval_end(mtl_signpost_log(), sp_post, "post");
    mtl_gpu_capture_end(s_cmdbuf);   /* W3.E1: flush the .gputrace (no-op unless capturing) */
    s_drawable = nil;
    s_cmdbuf = nil;
  }
}

static void mtl_finish_render(void) {
    /* Present is committed in end_frame; readback issues its own waited cmdbuf. */
}

static void mtl_on_resize(void) {
    /* Targets are re-created lazily in start_frame when drawableSize changes. */
}

/* ---- Non-vtable couplings (called directly from gfx_pc.c, backend-aware) --- */

extern "C" void gfx_metal_set_clear_color(float r, float g, float b) {
    s_clear_r = (double)r;
    s_clear_g = (double)g;
    s_clear_b = (double)b;
}

extern "C" int gfx_metal_max_offscreen_dim(void) {
    return 16384;  /* Apple GPU max 2D texture dimension; matches Apple GL's GL_MAX_TEXTURE_SIZE */
}

/* ---- Vtable: state setters (record only) + textures + draw ---------------- */

static bool mtl_z_is_from_0_to_1(void) { return true; }

static uint32_t mtl_new_texture(void) {
    static uint32_t next_id = 1;  /* nonzero, monotonic — 0 reads as "unset" */
    if (next_id == 0) next_id = 1;  /* skip the reserved 0 on the (2^32) wrap */
    return next_id++;
}

static void mtl_delete_texture(uint32_t texture_id) {
    if (texture_id != 0 && s_textures != nil) {
        [s_textures removeObjectForKey:@(texture_id)];
    }
}

static void mtl_select_texture(int tile, uint32_t texture_id) {
    if (tile >= 0 && tile < 2) s_tile_tex[tile] = texture_id;
    s_last_selected_id = texture_id;  /* upload target = most-recently selected id */
}

static bool mtl_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
  @autoreleasepool {
    /* Reject >4096 to match gfx_opengl_upload_texture's guard: an oversized HD
     * asset must fall through to the frontend's native-texel fallback on BOTH
     * backends (gfx_pc.c:21067), or GL and Metal diverge. */
    if (rgba32_buf == NULL || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return false;
    }
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width height:height mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [s_device newTextureWithDescriptor:d];
    if (tex == nil) return false;
    [tex replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0
             withBytes:rgba32_buf bytesPerRow:(NSUInteger)width * 4];
    if (s_last_selected_id != 0 && s_textures != nil) {
        s_textures[@(s_last_selected_id)] = tex;
    }
    return true;
  }
}

static void mtl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    if (tile < 0 || tile >= 2) return;
    s_tile_linear[tile] = linear_filter;
    s_tile_cms[tile] = cms;
    s_tile_cmt[tile] = cmt;
}

static void mtl_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare,
                               bool depth_source_prim, uint16_t zmode) {
    (void)depth_source_prim;
    s_depth_test = depth_test;
    s_depth_update = depth_update;
    s_depth_compare = depth_compare;
    s_zmode = zmode;
}

static void mtl_set_viewport(int x, int y, int width, int height) {
    s_vp_x = x;
    s_vp_y = y;
    s_vp_w = width;
    s_vp_h = height;
    static int dbg = -1;  /* cache like the other env checks (was a per-call getenv) */
    if (dbg < 0) dbg = getenv("GE007_METAL_DEBUG_VP") ? 1 : 0;
    if (dbg) {
        static int n = 0;
        if (n++ < 16)
            fprintf(stderr, "[metal-vp] set_viewport(%d,%d,%d,%d) fb=%dx%d flipY=%d\n",
                    x, y, width, height, s_fb_w, s_fb_h, s_fb_h - (y + height));
    }
}

static void mtl_set_scissor(int x, int y, int width, int height) {
    s_sc_x = x;
    s_sc_y = y;
    s_sc_w = width;
    s_sc_h = height;
    s_sc_set = true;
}

/* Mirror gfx_opengl.c:310-322 — default ON unless disabled by env. */
static bool mtl_room_cvg_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *d = getenv("GE007_DISABLE_ROOM_XLU_CVG_MEMORY");
        const char *e = getenv("GE007_ROOM_XLU_CVG_MEMORY");
        cached = 1;
        if ((d != NULL && d[0] != '\0' && d[0] != '0') || (e != NULL && e[0] == '0')) cached = 0;
    }
    return cached != 0;
}
/* Mirror gfx_opengl.c:300-308 — diag override (default off). */
static bool mtl_diag_cvg_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("GE007_DIAG_XLU_RDP_CVG_MEMORY_BLEND_CC");
        cached = (e != NULL && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return cached != 0;
}

static void mtl_set_blend_mode(enum GfxBlendMode mode) {
    s_blend = mode;
    /* Coverage-alpha preservation (gfx_opengl.c:1692-1703): when the
     * coverage-memory feature is active, ordinary translucent draws must NOT
     * overwrite the 3-bit coverage the RDP-CVG-memory shader stores in the
     * scene-target alpha channel. Metal always renders to the sampleable
     * offscreen (== GL's scene target, which is bound-by-default because
     * room_xlu_cvg_memory defaults on, gfx_opengl.c:2277), so gate on the
     * feature + the same four blend modes GL masks. Resolved into the PSO
     * colorWriteMask at draw time. */
    s_preserve_cov_alpha =
        (mtl_room_cvg_enabled() || mtl_diag_cvg_enabled()) &&
        (mode == GFX_BLEND_ALPHA || mode == GFX_BLEND_MODULATE ||
         mode == GFX_BLEND_ALPHA_COVERAGE || mode == GFX_BLEND_ALPHA_CVG_WRAP_STENCIL);
}

/* Union of the batch's triangle screen rects in GL (bottom-left) viewport space,
 * ported from gfx_opengl_compute_batch_snapshot_rect. Only valid for the
 * coverage-memory path, whose VBO carries the flat diag-tri NDC at floats 4..9
 * of each triangle's first vertex (stride >= 10). Returns false if unusable. */
static bool mtl_rdp_batch_rect_cvg(const float *buf_vbo, size_t buf_vbo_len,
                                   size_t num_tris, int out[4]) {
    if (buf_vbo == NULL || num_tris == 0) return false;
    size_t vcount = num_tris * 3;
    if (vcount == 0 || buf_vbo_len % vcount != 0) return false;
    size_t stride = buf_vbo_len / vcount;
    if (stride < 10) return false;
    const int margin = 3;
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool any = false;
    for (size_t tri = 0; tri < num_tris; tri++) {
        const float *base = &buf_vbo[tri * 3 * stride];
        for (int i = 0; i < 3; i++) {
            float ndc_x = base[4 + i * 2 + 0];
            float ndc_y = base[4 + i * 2 + 1];
            if (ndc_x != ndc_x || ndc_y != ndc_y ||
                ndc_x <= -100000.0f || ndc_x >= 100000.0f ||
                ndc_y <= -100000.0f || ndc_y >= 100000.0f) return false;
            float px = (float)s_vp_x + (ndc_x * 0.5f + 0.5f) * (float)s_vp_w;
            float py = (float)s_vp_y + (ndc_y * 0.5f + 0.5f) * (float)s_vp_h;
            if (!any) { min_x = max_x = px; min_y = max_y = py; any = true; }
            else {
                if (px < min_x) min_x = px; if (px > max_x) max_x = px;
                if (py < min_y) min_y = py; if (py > max_y) max_y = py;
            }
        }
    }
    if (!any) return false;
    /* floor(min)/ceil(max) via casts (avoid pulling <math.h> into this TU). */
    int ix0 = (int)min_x; if (min_x < (float)ix0) ix0--;
    int iy0 = (int)min_y; if (min_y < (float)iy0) iy0--;
    int ix1 = (int)max_x; if (max_x > (float)ix1) ix1++;
    int iy1 = (int)max_y; if (max_y > (float)iy1) iy1++;
    out[0] = ix0 - margin;
    out[1] = iy0 - margin;
    out[2] = ix1 + margin - out[0];
    out[3] = iy1 + margin - out[1];
    return out[2] > 0 && out[3] > 0;
}

static void mtl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
  @autoreleasepool {
    if (s_enc == nil || s_cur_shader == nil || buf_vbo_num_tris == 0 || buf_vbo == NULL) return;
    MetalShader *ms = s_cur_shader;

    /* Scissor: GL(bottom-left) -> Metal(top-left) Y-flip + clamp to attachment.
     * Compute first so a degenerate/off-screen rect (inactive split-screen pane,
     * fully-clipped HUD element) early-outs — GL keeps GL_SCISSOR_TEST on and
     * discards every fragment, so skipping the whole draw is equivalent, avoids
     * leaking the previous draw's scissor on the persistent encoder, and skips
     * the wasted snapshot + vertex copy. */
    int sx = s_sc_set ? s_sc_x : 0;
    int sy = s_sc_set ? s_sc_y : 0;
    int sw = s_sc_set ? s_sc_w : s_fb_w;
    int sh = s_sc_set ? s_sc_h : s_fb_h;
    int myTop = s_fb_h - (sy + sh);
    if (myTop < 0) { sh += myTop; myTop = 0; }
    if (sx < 0) { sw += sx; sx = 0; }
    if (sx + sw > s_fb_w) sw = s_fb_w - sx;
    if (myTop + sh > s_fb_h) sh = s_fb_h - myTop;
    if (sw <= 0 || sh <= 0) return;

    /* RDP-memory / coverage-memory blend: the fragment samples the framebuffer
     * "memory" color it blends against. Metal can't sample the attachment being
     * written, so end the encoder, blit-copy the region the batch samples into
     * the SAME origin of the sampled snapshot, and resume with loadAction=Load
     * (§4 risk #1 — no LUS template). Copy only the batch rect (coverage) or the
     * viewport (plain memory blend) instead of the whole framebuffer. */
    bool rdp_mem = (ms->diagRdpMemory || ms->diagRdpCvgMemory) &&
                   (s_blend == GFX_BLEND_ALPHA_RDP_MEMORY ||
                    s_blend == GFX_BLEND_ALPHA_RDP_CVG_MEMORY);
    if (rdp_mem && s_snapshot_tex != nil) {
        int rect[4];
        int rx, ry, rw, rh;
        if (ms->diagRdpCvgMemory &&
            mtl_rdp_batch_rect_cvg(buf_vbo, buf_vbo_len, buf_vbo_num_tris, rect)) {
            rx = rect[0]; ry = rect[1]; rw = rect[2]; rh = rect[3];
        } else {
            rx = s_vp_x; ry = s_vp_y; rw = s_vp_w; rh = s_vp_h;
        }
        /* clamp to viewport (GL space) */
        int vx0 = s_vp_x, vy0 = s_vp_y, vx1 = s_vp_x + s_vp_w, vy1 = s_vp_y + s_vp_h;
        int cx0 = rx < vx0 ? vx0 : rx, cy0 = ry < vy0 ? vy0 : ry;
        int cx1 = (rx + rw) > vx1 ? vx1 : (rx + rw), cy1 = (ry + rh) > vy1 ? vy1 : (ry + rh);
        rx = cx0; ry = cy0; rw = cx1 - cx0; rh = cy1 - cy0;
        int top = s_fb_h - (ry + rh);   /* Y-flip */
        if (rx < 0) { rw += rx; rx = 0; }
        if (top < 0) { rh += top; top = 0; }
        if (rx + rw > s_fb_w) rw = s_fb_w - rx;
        if (top + rh > s_fb_h) rh = s_fb_h - top;
        [s_enc endEncoding];
        if (rw > 0 && rh > 0) {
            id<MTLBlitCommandEncoder> b = [s_cmdbuf blitCommandEncoder];
            [b copyFromTexture:s_scene_color sourceSlice:0 sourceLevel:0
                  sourceOrigin:MTLOriginMake(rx, top, 0) sourceSize:MTLSizeMake(rw, rh, 1)
                     toTexture:s_snapshot_tex destinationSlice:0 destinationLevel:0
             destinationOrigin:MTLOriginMake(rx, top, 0)];
            [b endEncoding];
        }
        mtl_open_scene_encoder(false);
    }

    mtl_build_vertex_descriptor(ms);

    /* FID-0018: this is the ONLY pipeline that renders into the scene pass, so it
     * carries the pass's rasterSampleCount (s_msaa_samples); the shadow/SSAO/
     * post/minimap pipelines render into their own single-sample passes and stay
     * at 1. Alpha-to-coverage engages under MSAA for alpha-tested cutout surfaces
     * (blend-disabled texture-edge, mirroring GL's cutout_a2c) and the XLU
     * coverage diagnostic blend (GFX_BLEND_ALPHA_COVERAGE == GL's
     * g_blend_alpha_coverage), gated by the GE007_NO_A2C negative control.
     * Cutout A2C engages only on the opaque (blend-DISABLED) path, matching GL's
     * `g_blend_disabled` gate exactly — `!blend_on` would also fire on the
     * RDP-memory coverage diagnostic blends, which GL never treats as cutout. */
    bool cutout_a2c = (s_blend == GFX_BLEND_DISABLED) &&
                      ms->cc.opt_texture_edge && ms->cc.opt_alpha;
    bool a2c = mtl_msaa_active() && mtl_a2c_enabled() &&
               (cutout_a2c || s_blend == GFX_BLEND_ALPHA_COVERAGE);
    id<MTLRenderPipelineState> pso = mtl_pso_for(ms, s_blend, s_msaa_samples, a2c,
                                                 !s_preserve_cov_alpha);
    [s_enc setRenderPipelineState:pso];
    [s_enc setDepthStencilState:mtl_depth_state_for(s_depth_test, s_depth_update, s_depth_compare, s_zmode)];
    /* ZMODE_DEC decal polygon offset (gfx_opengl.c:1582-1595, factor/units -2). */
    if (s_zmode == 0xc00 && s_depth_test && s_depth_compare) {
        [s_enc setDepthBias:-7.5e-6f slopeScale:-2.0f clamp:0.0f];
    } else {
        [s_enc setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
    }

    /* Viewport: Y-flip + non-negative clamp (0 is legal and reproduces GL's
     * collapse; a negative extent would abort). Unconditional so a zero-extent
     * viewport doesn't leak the previous draw's viewport. */
    MTLViewport vp;
    vp.originX = s_vp_x;
    vp.originY = s_fb_h - (s_vp_y + s_vp_h);
    vp.width = s_vp_w > 0 ? (double)s_vp_w : 0.0;
    vp.height = s_vp_h > 0 ? (double)s_vp_h : 0.0;
    vp.znear = 0.0;
    vp.zfar = 1.0;
    [s_enc setViewport:vp];

    MTLScissorRect sc = {(NSUInteger)sx, (NSUInteger)myTop, (NSUInteger)sw, (NSUInteger)sh};
    [s_enc setScissorRect:sc];

    /* Vertex data: bump-allocate from the frame's ring slot (StorageModeShared);
     * fall back to a one-off buffer only if the batch overflows the slot. */
    size_t need = buf_vbo_len * sizeof(float);
    id<MTLBuffer> vb;
    NSUInteger voff;
    size_t aligned = (s_vbuf_cursor + 255u) & ~(size_t)255u;
    if (s_vbuf[s_vbuf_slot] != nil && aligned + need <= s_vbuf_cap[s_vbuf_slot]) {
        vb = s_vbuf[s_vbuf_slot];
        voff = (NSUInteger)aligned;
        memcpy((uint8_t *)vb.contents + aligned, buf_vbo, need);
        s_vbuf_cursor = aligned + need;
        if (s_vbuf_cursor > s_vbuf_want) s_vbuf_want = s_vbuf_cursor;  /* grow target for next (re)alloc */
    } else {
        if (aligned + need > s_vbuf_want) s_vbuf_want = aligned + need;
        vb = [s_device newBufferWithBytes:buf_vbo length:need options:MTLResourceStorageModeShared];
        voff = 0;
    }
    if (vb == nil) return;  /* transient OOM: drop this batch rather than GPU-fault */
    [s_enc setVertexBuffer:vb offset:voff atIndex:0];

    MtlUniforms u;
    memset(&u, 0, sizeof u);
    u.n64FilterScale[0] = mtl_axis_filter_scale(gfx_current_dimensions.width, viGetX(), DESIRED_SCREEN_WIDTH);
    u.n64FilterScale[1] = mtl_axis_filter_scale(gfx_current_dimensions.height, viGetY(), DESIRED_SCREEN_HEIGHT);
    u.frameCount = (int)s_frame_count_metal;
    u.winH = s_vp_h > 0 ? s_vp_h : s_fb_h;   /* GL current_height = viewport height (noise scale) */
    u.fbH = s_fb_h;                          /* attachment height (fragcoord flip) */
    /* RDP diag uniforms: full-fb snapshot -> origin 0 (memoryUv = in.position/fb);
     * diagViewport is the GL(bottom-left) viewport for the coverage NDC
     * reconstruction, which pairs with the GL-oriented flipped fragCoord. */
    u.diagFbOrigin[0] = 0.0f;
    u.diagFbOrigin[1] = 0.0f;
    u.diagViewport[0] = (float)s_vp_x;
    u.diagViewport[1] = (float)s_vp_y;
    u.diagViewport[2] = (float)s_vp_w;
    u.diagViewport[3] = (float)s_vp_h;
    /* W1.E3.T4: sun-shadow receiver uniforms (unused fields stay zero from memset). */
    if (ms->cc.opt_sun_shadow) {
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                u.shadowMat[c * 4 + r] = g_pc_shadow_mat[r][c];   /* column-major transpose */
        float t = (s_shadow_res > 0) ? (1.0f / (float)s_shadow_res) : (1.0f / 2048.0f);
        u.shadowTexel[0] = t; u.shadowTexel[1] = t;
        u.shadowBias = g_pcSunShadowBias;
        u.shadowUmbra = g_pcSunShadowUmbra;
    }
    if (ms->cc.opt_dfdx_light) {
        u.sunColorLinear[0] = g_pc_sun_color_linear[0];
        u.sunColorLinear[1] = g_pc_sun_color_linear[1];
        u.sunColorLinear[2] = g_pc_sun_color_linear[2];
        u.sunColorLinear[3] = 0.0f;
        u.sunStrength = g_pcSunStrength;
    }
    [s_enc setFragmentBytes:&u length:sizeof u atIndex:1];

    for (int t = 0; t < 2; t++) {
        if (!ms->usedTextures[t]) continue;
        id<MTLTexture> tex = mtl_lookup_tex(s_tile_tex[t]);
        if (tex == nil) tex = s_white_tex;
        [s_enc setFragmentTexture:tex atIndex:t];
        [s_enc setFragmentSamplerState:mtl_sampler_for(s_tile_linear[t], s_tile_cms[t], s_tile_cmt[t]) atIndex:t];
    }
    if (rdp_mem) {
        [s_enc setFragmentTexture:s_snapshot_tex atIndex:2];
        [s_enc setFragmentSamplerState:s_snapshot_sampler atIndex:2];
    }
    if (ms->cc.opt_sun_shadow) {
        /* The shader declares texture(5)/sampler(5), so they must always be bound or
         * Metal faults. Ensure the resources exist (start_frame's shadow pass builds
         * them, but on the very first flagged frame the bit can be set mid-frame
         * before that ran); fall back to the cleared 1x1 dummy (never s_scene_depth
         * — that's this encoder's live depth ATTACHMENT, and binding it as a
         * fragment texture here too is a read-while-write hazard) if the shadow
         * target is somehow still nil (1-frame "fully lit", transient — in the
         * gated invariant this branch is defensive-only, see mtl_ensure_shadow_dummy). */
        mtl_ensure_shadow_resources(g_pcSunShadowRes);
        id<MTLTexture> smap = s_shadow_depth;
        if (smap == nil) {
            static int s_shadow_dummy_fallback_logged = 0;
            if (!s_shadow_dummy_fallback_logged) {
                s_shadow_dummy_fallback_logged = 1;
                fprintf(stderr, "[metal][RENDER-HEALTH] shadow receiver active with no "
                                "shadow-map resource this frame; using cleared dummy "
                                "(fully-lit) fallback\n");
            }
            if (mtl_shadow_dummy_depth_disabled()) {
                /* Negative control: revert to the pre-fix read-while-write hazard
                 * (bind the live scene depth attachment as the shadow sampler). */
                smap = s_scene_depth;
            } else {
                mtl_ensure_shadow_dummy();
                smap = s_shadow_dummy_depth;
            }
        }
        if (smap != nil && s_shadow_cmp_sampler != nil) {
            [s_enc setFragmentTexture:smap atIndex:5];                /* §4.4: unit 5 */
            [s_enc setFragmentSamplerState:s_shadow_cmp_sampler atIndex:5];
        }
    }

    [s_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 * buf_vbo_num_tris];
  }
}

/* ---- readback (3.4): blit offscreen -> shared buffer, wait, RGB out -------
 * Returns bytes in GL's convention (bottom-left origin, row 0 = bottom) so the
 * existing screenshot/probe consumers (calibrated to glReadPixels) are
 * unchanged. Source region is Y-flipped into the top-left scene texture. */
static bool mtl_read_framebuffer_rgb(int x, int y, int width, int height, uint8_t *rgb_out) {
  @autoreleasepool {
    if (rgb_out == NULL || width <= 0 || height <= 0 || s_scene_color == nil || s_device == nil) {
        return false;
    }
    /* Source selection matches GL: a MID-frame read (GE007_SCREENSHOT / diag
     * pixel probes, encoder still open) sees the current scene BEFORE the
     * output filter (GL's probes read the scene FBO mid-render), so read
     * s_scene_color; a BETWEEN-frame read (main screenshot, encoder closed)
     * sees the last PRESENTED image (GL reads its post-filter GL_FRONT), so read
     * s_readback_src (== s_final_color when the filter ran, else s_scene_color). */
    bool mid_frame = (s_enc != nil && s_cmdbuf != nil);
    id<MTLTexture> src_tex = mid_frame ? s_scene_color
                                       : (s_readback_src != nil ? s_readback_src : s_scene_color);
    if (mid_frame) {
        /* Flush the in-flight draws so the readback sees THIS frame's content,
         * then resume the frame (Load-preserved). Metal serializes command
         * buffers by commit order, so without this the read would capture the
         * previous committed frame. */
        [s_enc endEncoding];
        s_enc = nil;
        [s_cmdbuf commit];
        [s_cmdbuf waitUntilCompleted];
        s_cmdbuf = [s_queue commandBuffer];
        mtl_open_scene_encoder(false);
    }
    if (src_tex == nil) return false;
    /* Clamp the requested rect to the attachment. */
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > s_fb_w) width = s_fb_w - x;
    if (y + height > s_fb_h) height = s_fb_h - y;
    if (width <= 0 || height <= 0) return false;

    int src_top = s_fb_h - (y + height);   /* GL bottom-left y -> Metal top-left origin */
    if (src_top < 0) src_top = 0;

    NSUInteger bpr = (NSUInteger)width * 4;
    id<MTLBuffer> buf = [s_device newBufferWithLength:bpr * (NSUInteger)height
                                             options:MTLResourceStorageModeShared];
    if (buf == nil) return false;

    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:src_tex sourceSlice:0 sourceLevel:0
          sourceOrigin:MTLOriginMake(x, src_top, 0)
            sourceSize:MTLSizeMake(width, height, 1)
              toBuffer:buf destinationOffset:0
     destinationBytesPerRow:bpr destinationBytesPerImage:bpr * (NSUInteger)height];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        /* A failed blit leaves buf.contents undefined; report failure so the
         * probe/screenshot consumers match GL's `return err == GL_NO_ERROR`. */
        fprintf(stderr, "[metal] readback command buffer failed: %s\n",
                cb.error ? cb.error.localizedDescription.UTF8String : "(unknown)");
        return false;
    }

    const uint8_t *src = (const uint8_t *)buf.contents;  /* BGRA, top-to-bottom */
    for (int row = 0; row < height; row++) {
        /* rgb_out row r is GL bottom-up; Metal buffer row 0 = top of region. */
        const uint8_t *srow = src + (size_t)(height - 1 - row) * bpr;
        uint8_t *drow = rgb_out + (size_t)row * (size_t)width * 3;
        for (int col = 0; col < width; col++) {
            drow[col * 3 + 0] = srow[col * 4 + 2];  /* R (BGRA -> RGB) */
            drow[col * 3 + 1] = srow[col * 4 + 1];  /* G */
            drow[col * 3 + 2] = srow[col * 4 + 0];  /* B */
        }
    }
    return true;
  }
}

extern "C" bool gfx_metal_get_framebuffer_size(int *width, int *height) {
    if (width == NULL || height == NULL || s_fb_w <= 0 || s_fb_h <= 0) {
        return false;
    }
    *width = s_fb_w;
    *height = s_fb_h;
    return true;
}

/* Positional init MUST match the field order in gfx_rendering_api.h. C linkage
 * so gfx_pc.c's `extern struct GfxRenderingAPI gfx_metal_api;` resolves. */
/* ------------------------------------------------------------------------
 * Modern-mesh draw path (W9 scene decoration, G_MODERNMESH).
 *
 * Draws a full-fidelity mesh (float32 verts, u32 indices, mipmapped RGBA8
 * texture of arbitrary size) directly into the live scene encoder, at the
 * exact DL position the interpreter reached — same color/depth attachments,
 * same (persisting) viewport/scissor as the surrounding N64 draws. The GPU
 * transforms with the interpreter's MP matrix; the vertex shader replicates
 * the N64 per-vertex fog curve and Metal's 0..1 depth remap
 * (mtl_z_is_from_0_to_1 == true, so clip.z = (z+w)/2 like the CPU T&L).
 * PSOs are cached per (cutout, sample count) — the scene pass carries
 * s_msaa_samples, and a mismatched rasterSampleCount GPU-faults (FID-0018).
 * GPU resources per mesh upload once (private-ish shared buffers + a one-off
 * blit command buffer for mipmap generation, which cannot run on s_cmdbuf
 * while the scene encoder is open) and cache in s_modern_cache.
 * ---------------------------------------------------------------------- */

struct MtlModernUniforms {
    float mvp[16];       /* row-major MP; memcpy into MSL float4x4 gives
                            columns == MP rows, so u.mvp * v == the CPU
                            row-vector T&L exactly */
    float fog[4];        /* rgb + pad */
    float fogMul, fogOffset, fogOn, pad;
};
static_assert(sizeof(MtlModernUniforms) == 96, "MSL DecorUniforms layout");

static NSMutableDictionary<NSNumber *, NSArray *> *s_modern_cache = nil;
static id<MTLRenderPipelineState> s_modern_pso[2][2] = {};  /* [cutout][msaa>1] */
static int s_modern_pso_samples[2][2] = {};
static id<MTLSamplerState> s_modern_sampler = nil;

static const char *kModernMeshMSL =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct DecorVertex {\n"
    "  float3 pos [[attribute(0)]];\n"
    "  float3 nrm [[attribute(1)]];\n"
    "  float2 uv  [[attribute(2)]];\n"
    "  float4 col [[attribute(3)]];\n"
    "};\n"
    "struct DecorUniforms { float4x4 mvp; float4 fog; float fogMul; float fogOffset; float fogOn; float pad; };\n"
    "struct DecorOut { float4 position [[position]]; float2 uv; float4 col; float fogA; };\n"
    "vertex DecorOut decorVertex(DecorVertex in [[stage_in]],\n"
    "                            constant DecorUniforms& u [[buffer(1)]]) {\n"
    "  float4 clip = u.mvp * float4(in.pos, 1.0);\n"
    "  float fogA = 0.0;\n"
    "  if (u.fogOn > 0.5) {\n"    /* N64 fog curve: gfx_fog_coord_from_clip */
    "    float ww = clip.w;\n"
    "    if (fabs(ww) < 0.001) ww = 0.001;\n"
    "    float winv = 1.0 / ww;\n"
    "    float coord = (winv < 0.0) ? clip.z * 32767.0 : clip.z * winv;\n"
    "    fogA = clamp(coord * u.fogMul + u.fogOffset, 0.0, 255.0) / 255.0;\n"
    "  }\n"
    "  DecorOut o;\n"
    "  clip.z = (clip.z + clip.w) * 0.5;\n"   /* Metal 0..1 depth remap */
    "  o.position = clip; o.uv = in.uv; o.col = in.col; o.fogA = fogA;\n"
    "  return o;\n"
    "}\n"
    /* COLOR_0.rgb = baked light; COLOR_0.a = baked SNOW COVER (0 = none),
     * painted toward a cool snow white before fog. Cutout discard keys on
     * the TEXTURE alpha alone -- vertex alpha is snow, not opacity. */
    "static float4 decorShade(DecorOut in, float4 t, constant DecorUniforms& u) {\n"
    "  float3 c = t.rgb * in.col.rgb;\n"
    "  c = mix(c, float3(0.88, 0.91, 0.96), in.col.a);\n"
    "  c = mix(c, u.fog.rgb, in.fogA);\n"
    "  return float4(c, t.a);\n"
    "}\n"
    "fragment float4 decorFragmentOpaque(DecorOut in [[stage_in]],\n"
    "    texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]],\n"
    "    constant DecorUniforms& u [[buffer(1)]]) {\n"
    "  float4 c = decorShade(in, tex.sample(smp, in.uv), u);\n"
    "  return float4(c.rgb, 1.0);\n"
    "}\n"
    "fragment float4 decorFragmentCutout(DecorOut in [[stage_in]],\n"
    "    texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]],\n"
    "    constant DecorUniforms& u [[buffer(1)]]) {\n"
    "  float4 t = tex.sample(smp, in.uv);\n"
    "  if (t.a < 0.45) discard_fragment();\n"
    "  float4 c = decorShade(in, t, u);\n"
    "  return float4(c.rgb, 1.0);\n"
    "}\n";

static bool mtl_ensure_modern_pso(int cutout, int samples) {
    int msaa = samples > 1 ? 1 : 0;
    if (s_modern_pso[cutout][msaa] != nil &&
        s_modern_pso_samples[cutout][msaa] == samples) {
        return true;
    }
    if (s_device == nil) return false;

  @autoreleasepool {
    NSError *err = nil;
    NSString *nssrc = [NSString stringWithUTF8String:kModernMeshMSL];
    id<MTLLibrary> lib = [s_device newLibraryWithSource:nssrc options:nil error:&err];
    if (lib == nil) {
        fprintf(stderr, "[metal] modern-mesh MSL compile failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
        return false;
    }
    id<MTLFunction> vfn = [lib newFunctionWithName:@"decorVertex"];
    id<MTLFunction> ffn = [lib newFunctionWithName:cutout ? @"decorFragmentCutout"
                                                          : @"decorFragmentOpaque"];
    if (vfn == nil || ffn == nil) return false;

    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat3;
    vd.attributes[0].offset = 0;
    vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat3;
    vd.attributes[1].offset = 12;
    vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format = MTLVertexFormatFloat2;
    vd.attributes[2].offset = 24;
    vd.attributes[2].bufferIndex = 0;
    vd.attributes[3].format = MTLVertexFormatUChar4Normalized;
    vd.attributes[3].offset = 32;
    vd.attributes[3].bufferIndex = 0;
    vd.layouts[0].stride = 36;
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    vd.layouts[0].stepRate = 1;

    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vfn;
    pd.fragmentFunction = ffn;
    pd.vertexDescriptor = vd;
    pd.rasterSampleCount = samples;   /* MUST match the scene pass (FID-0018) */
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.colorAttachments[0].blendingEnabled = NO;
    if (cutout && samples > 1) {
        pd.alphaToCoverageEnabled = YES;  /* soften cutout edges under MSAA */
    }
    s_modern_pso[cutout][msaa] =
        [s_device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (s_modern_pso[cutout][msaa] == nil) {
        fprintf(stderr, "[metal] modern-mesh PSO build failed: %s\n",
                err ? err.localizedDescription.UTF8String : "(no error)");
        return false;
    }
    s_modern_pso_samples[cutout][msaa] = samples;
    return true;
  }
}

static NSArray *mtl_modern_mesh_resources(struct GfxModernMesh *mesh) {
    if (s_modern_cache == nil) {
        s_modern_cache = [NSMutableDictionary new];
    }
    NSNumber *key = @(mesh->mesh_id);
    NSArray *res = s_modern_cache[key];
    if (res != nil) return res;
    if (s_modern_cache.count > 64) {
        /* level churn eviction: ids are never reused, so dropping the whole
           cache is safe -- live meshes re-upload once on next draw */
        [s_modern_cache removeAllObjects];
    }

    id<MTLBuffer> vb = [s_device newBufferWithBytes:mesh->vtx
                                             length:(NSUInteger)mesh->vtx_count * 36
                                            options:MTLResourceStorageModeShared];
    id<MTLBuffer> ib = [s_device newBufferWithBytes:mesh->idx
                                             length:(NSUInteger)mesh->idx_count * 4
                                            options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                     width:(NSUInteger)mesh->tex_w
                                    height:(NSUInteger)mesh->tex_h
                                 mipmapped:YES];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [s_device newTextureWithDescriptor:td];
    if (vb == nil || ib == nil || tex == nil) return nil;
    [tex replaceRegion:MTLRegionMake2D(0, 0, mesh->tex_w, mesh->tex_h)
           mipmapLevel:0
             withBytes:mesh->tex_rgba
           bytesPerRow:(NSUInteger)mesh->tex_w * 4];
    /* Mip generation on a ONE-OFF command buffer: a blit encoder cannot run
     * on s_cmdbuf while the scene render encoder is open. Happens once per
     * mesh at first draw. */
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit generateMipmapsForTexture:tex];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    res = @[ vb, ib, tex ];
    s_modern_cache[key] = res;
    return res;
}

static void mtl_draw_modern_mesh(struct GfxModernMesh *mesh,
                                 const float mvp[4][4],
                                 const float fog_color[3], float fog_mul,
                                 float fog_offset, int fog_enabled) {
    if (s_enc == nil || s_device == nil || mesh == NULL ||
        mesh->vtx == NULL || mesh->idx == NULL || mesh->tex_rgba == NULL) {
        return;
    }
    int cutout = mesh->cutout ? 1 : 0;
    if (!mtl_ensure_modern_pso(cutout, s_msaa_samples)) {
        return;
    }
    NSArray *res = mtl_modern_mesh_resources(mesh);
    if (res == nil) return;
    if (s_modern_sampler == nil) {
        MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterLinear;
        sd.maxAnisotropy = 16;
        sd.sAddressMode = MTLSamplerAddressModeRepeat;
        sd.tAddressMode = MTLSamplerAddressModeRepeat;
        s_modern_sampler = [s_device newSamplerStateWithDescriptor:sd];
    }

    MtlModernUniforms u;
    memset(&u, 0, sizeof u);
    memcpy(u.mvp, mvp, sizeof u.mvp);
    u.fog[0] = fog_color[0];
    u.fog[1] = fog_color[1];
    u.fog[2] = fog_color[2];
    u.fogMul = fog_mul;
    u.fogOffset = fog_offset;
    u.fogOn = fog_enabled ? 1.0f : 0.0f;

    int msaa = s_msaa_samples > 1 ? 1 : 0;
    [s_enc setRenderPipelineState:s_modern_pso[cutout][msaa]];
    [s_enc setDepthStencilState:mtl_depth_state_for(true, true, true, 0)];
    [s_enc setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
    [s_enc setCullMode:MTLCullModeNone];  /* cutout cards are two-sided */
    [s_enc setVertexBuffer:(id<MTLBuffer>)res[0] offset:0 atIndex:0];
    [s_enc setVertexBytes:&u length:sizeof u atIndex:1];
    [s_enc setFragmentBytes:&u length:sizeof u atIndex:1];
    [s_enc setFragmentTexture:(id<MTLTexture>)res[2] atIndex:0];
    [s_enc setFragmentSamplerState:s_modern_sampler atIndex:0];
    [s_enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:mesh->idx_count
                       indexType:MTLIndexTypeUInt32
                     indexBuffer:(id<MTLBuffer>)res[1]
               indexBufferOffset:0];
}

extern "C" struct GfxRenderingAPI gfx_metal_api = {
    .z_is_from_0_to_1 = mtl_z_is_from_0_to_1,
    .unload_shader = mtl_unload_shader,
    .load_shader = mtl_load_shader,
    .create_and_load_new_shader = mtl_create_and_load_new_shader,
    .lookup_shader = mtl_lookup_shader,
    .shader_get_info = mtl_shader_get_info,
    .new_texture = mtl_new_texture,
    .delete_texture = mtl_delete_texture,
    .select_texture = mtl_select_texture,
    .upload_texture = mtl_upload_texture,
    .set_sampler_parameters = mtl_set_sampler_parameters,
    .set_depth_mode = mtl_set_depth_mode,
    .set_viewport = mtl_set_viewport,
    .set_scissor = mtl_set_scissor,
    .set_blend_mode = mtl_set_blend_mode,
    .draw_triangles = mtl_draw_triangles,
    .read_framebuffer_rgb = mtl_read_framebuffer_rgb,
    .init = mtl_init,
    .on_resize = mtl_on_resize,
    .start_frame = mtl_start_frame,
    .end_frame = mtl_end_frame,
    .finish_render = mtl_finish_render,
    .draw_modern_mesh = mtl_draw_modern_mesh,
    .upload_texture_mipped = NULL,
};
