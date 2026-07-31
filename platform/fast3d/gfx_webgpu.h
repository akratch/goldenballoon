/*
 * gfx_webgpu.h — public seam of the WebGPU (wgpu-native) render backend.
 *
 * Exposes the pieces of gfx_webgpu.c that the app shell (AppHost) needs to
 * create a WGPUSurface from the window it owns, so both the standalone engine
 * and the launcher build surfaces through one code path. Only meaningful when
 * MDKR_WEBGPU_BACKEND.
 */
#ifndef GFX_WEBGPU_H
#define GFX_WEBGPU_H

#include <stdbool.h>

#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Surface creation moved to the dialect seam wgpuCompatCreateSurface()
 * (gfx_webgpu_compat.h): native window descriptors on desktop, the
 * #canvas selector in the browser. gfx_webgpu_bringup() calls it
 * internally; external callers use bringup (below). */

/* Full WebGPU bring-up for a native window handle: instance -> surface ->
 * adapter -> device -> queue -> surface format. `metal_layer` is used on macOS
 * (a CAMetalLayer*), `sdl_window` (a SDL_Window*) everywhere else — pass both;
 * the unused one is ignored. On success returns true with every out-param set
 * and the caller owning the objects; on failure returns false (out-params
 * untouched). Shared by the engine and the app shell so the adapter/device
 * request sequence lives once. `out_format` is a WGPUTextureFormat as int. */
bool gfx_webgpu_bringup(void *metal_layer, void *sdl_window,
                        WGPUInstance *out_instance, WGPUAdapter *out_adapter,
                        WGPUDevice *out_device, WGPUQueue *out_queue,
                        WGPUSurface *out_surface, int *out_format);

/* True when DepthClipControl was granted and pipelines can clamp, rather than
 * clip, geometry beyond the far plane. The frontend performs the equivalent
 * homogeneous z clamp when this is false. */
bool gfx_webgpu_unclipped_depth_supported(void);
/* Exact completed-frame extent in OUTPUT space (the resolve target), not the
 * supersampled scene target. Used by the shared capture-size contract. */
bool gfx_webgpu_get_output_size(int *width, int *height);
/* Native-only live recovery. The callback/fault path only latches state;
 * platform_frame_sync consumes it between complete frames. */
bool gfx_webgpu_runtime_recovery_pending(void);
bool gfx_webgpu_recover_device(void);
void gfx_webgpu_prepare_gl_fallback(void);
/* Emit one end-of-run material-capacity census for headless/browser gates.
 * This is observational only and remains safe before bring-up or after a
 * controlled renderer failure. */
void gfx_webgpu_report_limits(void);
struct GfxRenderingAPI;
extern struct GfxRenderingAPI gfx_webgpu_api;

/* The in-game F1 overlay (ui_overlay.cpp) draws into the surface render pass
 * wgpu_end_frame opens just before present. gfx_webgpu_current_overlay_pass()
 * returns that WGPURenderPassEncoder (as void*, NULL when none is open) to pass
 * to gfx_webgpu_imgui_render; gfx_webgpu_current_overlay_size() returns its
 * pixel size for the ImGui framebuffer scale + scissor clamp. */
void *gfx_webgpu_current_overlay_pass(void);
void  gfx_webgpu_current_overlay_size(int *w, int *h);

/* PERF-005 Phase 2 (W4.3): record/replay pipeline prewarm. boss.c calls both on
 * every stage (re)load, inside the watchdog-suppressed load window:
 *   gfx_webgpu_set_stage(stage)     — point the recorder at `stage` (and persist the
 *                                     previous stage's manifest if it grew).
 *   gfx_webgpu_prewarm_stage(stage) — synchronously build every pipeline recorded for
 *                                     `stage` on a prior visit/session, so no material
 *                                     is cold on entry.
 * Both are safe no-ops when the webgpu backend isn't active, under --deterministic,
 * or when GE007_PIPECACHE=0. Persistence is best-effort (I/O failure = no prewarm). */
void gfx_webgpu_set_stage(int stage);
void gfx_webgpu_prewarm_stage(int stage);

#ifdef __cplusplus
}
#endif

#endif /* GFX_WEBGPU_H */
