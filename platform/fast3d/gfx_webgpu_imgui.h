/*
 * gfx_webgpu_imgui.h — minimal Dear ImGui renderer on WebGPU (wgpu-native v29).
 *
 * In-house replacement for upstream imgui_impl_wgpu, which is version-skewed
 * from our pinned wgpu-native v29 (its _WGPU path predates v29's nextInChain-
 * first structs; its _DAWN path is Emscripten-oriented). This renderer uses
 * v29's exact API and the same pipeline/bind-group patterns as gfx_webgpu.c.
 *
 * Shared by the C++ app shell (AppHost launcher UI) and the C in-game F1 overlay
 * (ui_overlay.cpp) — one ImGui context, one renderer. Implements ImGui 1.92's
 * dynamic-texture model (ImGuiBackendFlags_RendererHasTextures): the font atlas
 * grows glyphs on demand, so the renderer honors ImTextureData create/update/
 * destroy requests each frame (a one-time upload would drop later-added glyphs).
 *
 * Compiled as C++ (it consumes ImGui's ImDrawData) but exposes a C ABI.
 * Only meaningful when MGB64_WEBGPU_BACKEND.
 */
#ifndef GFX_WEBGPU_IMGUI_H
#define GFX_WEBGPU_IMGUI_H

#include <stdbool.h>

struct ImDrawData;

#ifdef __cplusplus
extern "C" {
#endif

/* Build the pipeline, font-atlas texture, sampler, and uniform/bind-group state.
 * device/queue are WGPUDevice/WGPUQueue (as void*); surface_format is the
 * WGPUTextureFormat the render pass targets. Returns false on failure. */
bool gfx_webgpu_imgui_init(void *device, void *queue, int surface_format);

/* Call once per ImGui frame before ImGui::NewFrame(). No-op in the legacy path;
 * kept for symmetry + future dynamic-texture support. */
void gfx_webgpu_imgui_new_frame(void);

/* Encode ImGui draw commands into an already-open render pass encoder
 * (WGPURenderPassEncoder as void*). fb_width/height are the target pixel size
 * (for scissor clamping). Safe to call with NULL/empty draw_data. */
void gfx_webgpu_imgui_render(struct ImDrawData *draw_data, void *render_pass_encoder,
                             int fb_width, int fb_height);

/* Release all GPU resources. */
void gfx_webgpu_imgui_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GFX_WEBGPU_IMGUI_H */
