/*
 * gfx_render_scale.h — one clamp for Video.RenderScale, shared by the frontend
 * and every backend.
 *
 * WHAT THIS FIXES. Video.RenderScale was inert in this port. The WebGPU backend
 * never read g_pcRenderScale at all, and the GL backend read it only to decide
 * *whether* a scene framebuffer should exist — never to size one
 * (gfx_opengl_scene_target_enabled(); ensure_scene_target() was called with the
 * unscaled drawable). So "2x supersampling" resized nothing and changed no
 * pixel on either backend.
 *
 * The sizing lives in mgb64's shared gfx_pc.c
 * (gfx_sync_current_dimensions_from_window sets gfx_current_dimensions =
 * window * RenderScale). This port replaced that frontend wholesale with
 * gfx_pc_dkr.c and the multiply was never carried across.
 *
 * The NaN guard is not decoration: NaN compares false against both bounds, so a
 * bare range test passes it through into target-sizing arithmetic.
 */
#ifndef MDKR64_GFX_RENDER_SCALE_H
#define MDKR64_GFX_RENDER_SCALE_H

#include <math.h>

#include "gfx_uniforms.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_RENDER_SCALE_MIN 1.0f
#define GFX_RENDER_SCALE_MAX 4.0f

/*
 * Keep the three full-resolution scene targets inside a predictable area
 * budget. A 2560x1440 fullscreen output at the default 2x scale otherwise
 * requests three 5120x2880 textures; transactional resize briefly retains the
 * old set too. That is legal by maxTextureDimension2D yet large enough to evict
 * or lose a lower-budget GPU, particularly through Linux browser drivers.
 *
 * This is deliberately a RENDER-pixel budget, not an output cap. Native output
 * stays pixel-perfect; only supersampling headroom is reduced. The browser uses
 * a tighter 1440p ceiling because its GPU process owns a swapchain and several
 * other copies outside our accounting; native backends retain a 4K ceiling.
 * Smaller windows still receive the requested 1x..4x scale.
 */
#ifdef __EMSCRIPTEN__
#define GFX_RENDER_PIXEL_BUDGET (2560u * 1440u)
#else
#define GFX_RENDER_PIXEL_BUDGET (3840u * 2160u)
#endif

static inline float gfx_effective_render_scale(void) {
    if (g_pcRenderScale != g_pcRenderScale) {   /* NaN */
        return GFX_RENDER_SCALE_MIN;
    }
    if (g_pcRenderScale < GFX_RENDER_SCALE_MIN) {
        return GFX_RENDER_SCALE_MIN;
    }
    if (g_pcRenderScale > GFX_RENDER_SCALE_MAX) {
        return GFX_RENDER_SCALE_MAX;
    }
    return g_pcRenderScale;
}

/*
 * Resolve a pair together so edge and area limits preserve the authored aspect
 * ratio. `max_dimension == 0` means the backend has not published an edge
 * limit yet. Output is assumed to have already been clamped to that limit; a
 * scale below 1x is never selected.
 */
static inline void gfx_render_scaled_dimensions(
    unsigned int output_width,
    unsigned int output_height,
    unsigned int max_dimension,
    unsigned int *render_width,
    unsigned int *render_height) {
    double scale = (double)gfx_effective_render_scale();
    double output_pixels;

    if (output_width == 0u) {
        output_width = 1u;
    }
    if (output_height == 0u) {
        output_height = 1u;
    }
    if (max_dimension > 0u) {
        double edge_scale =
            (double)max_dimension / (double)output_width;
        double height_scale =
            (double)max_dimension / (double)output_height;
        if (height_scale < edge_scale) {
            edge_scale = height_scale;
        }
        if (edge_scale < scale) {
            scale = edge_scale;
        }
    }
    output_pixels = (double)output_width * (double)output_height;
    if (output_pixels < (double)GFX_RENDER_PIXEL_BUDGET) {
        double area_scale =
            sqrt((double)GFX_RENDER_PIXEL_BUDGET / output_pixels);
        if (area_scale < scale) {
            scale = area_scale;
        }
    } else {
        scale = 1.0;
    }
    if (scale < 1.0) {
        scale = 1.0;
    }

    if (render_width != NULL) {
        *render_width =
            (unsigned int)floor((double)output_width * scale + 1.0e-9);
        if (*render_width == 0u) {
            *render_width = 1u;
        }
    }
    if (render_height != NULL) {
        *render_height =
            (unsigned int)floor((double)output_height * scale + 1.0e-9);
        if (*render_height == 0u) {
            *render_height = 1u;
        }
    }
}

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_GFX_RENDER_SCALE_H */
