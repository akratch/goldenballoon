#ifndef MDKR_GFX_WEBGPU_FAULT_H
#define MDKR_GFX_WEBGPU_FAULT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Stable WebGPU fault vocabulary. Keep the name user-facing: native and
 * browser gates select one point with MDKR_WEBGPU_FAULT=name[@occurrence].
 * Occurrences are one-based and counted independently for every point; @all
 * injects on every visit for bounded-retry tests.
 */
#define GFX_WEBGPU_FAULT_POINT_LIST(X) \
    X(BRINGUP_INSTANCE,           "bringup.instance") \
    X(BRINGUP_SURFACE,            "bringup.surface") \
    X(BRINGUP_ADAPTER,            "bringup.adapter") \
    X(BRINGUP_DEVICE_LIMITS,      "bringup.device-limits") \
    X(BRINGUP_DEVICE_DEFAULTS,    "bringup.device-defaults") \
    X(BRINGUP_QUEUE,              "bringup.queue") \
    X(CAPS_FORMAT,                "capabilities.format") \
    X(CAPS_ALPHA,                 "capabilities.alpha") \
    X(CAPS_PRESENT,               "capabilities.present") \
    X(CAPS_CONFIGURE,             "capabilities.configure") \
    X(CAPS_DEPTH_CLIP_ABSENT,     "capabilities.depth-clip-absent") \
    X(SURFACE_CONFIGURE,          "surface.configure") \
    X(SURFACE_SUBOPTIMAL,         "surface.suboptimal") \
    X(SURFACE_TIMEOUT,            "surface.timeout") \
    X(SURFACE_OUTDATED,           "surface.outdated") \
    X(SURFACE_LOST,               "surface.lost") \
    X(SURFACE_ERROR,              "surface.error") \
    X(DEVICE_LOST,                "device.lost") \
    X(QUEUE_SUBMIT,               "queue.submit") \
    X(NOISE_BUFFER,               "frame.noise-buffer") \
    X(SCENE_TEXTURE,              "target.scene-texture") \
    X(SCENE_VIEW,                 "target.scene-view") \
    X(DEPTH_TEXTURE,              "target.depth-texture") \
    X(DEPTH_VIEW,                 "target.depth-view") \
    X(POST_TEXTURE,               "target.post-texture") \
    X(POST_VIEW,                  "target.post-view") \
    X(RESOLVE_TEXTURE,            "target.resolve-texture") \
    X(RESOLVE_VIEW,               "target.resolve-view") \
    X(OUTPUT_DEPTH_TEXTURE,       "target.output-depth-texture") \
    X(OUTPUT_DEPTH_VIEW,          "target.output-depth-view") \
    X(FRAME_ENCODER,              "frame.encoder") \
    X(FRAME_PASS,                 "frame.pass") \
    X(FRAME_FINISH,               "frame.finish") \
    X(RESOLVE_MODULE,             "resolve.module") \
    X(RESOLVE_BGL,                "resolve.bind-group-layout") \
    X(RESOLVE_UNIFORM,            "resolve.uniform") \
    X(RESOLVE_SAMPLER,            "resolve.sampler") \
    X(RESOLVE_LAYOUT,             "resolve.pipeline-layout") \
    X(RESOLVE_PIPELINE,           "resolve.pipeline") \
    X(RESOLVE_BIND_GROUP,         "resolve.bind-group") \
    X(RESOLVE_PASS,               "resolve.pass") \
    X(OUTPUT_OVERLAY_PASS,        "overlay.output-pass") \
    X(POST_MODULE,                "post.module") \
    X(POST_BGL,                   "post.bind-group-layout") \
    X(POST_UNIFORM,               "post.uniform") \
    X(POST_SAMPLER_NEAREST,       "post.sampler-nearest") \
    X(POST_SAMPLER_LINEAR,        "post.sampler-linear") \
    X(POST_LAYOUT,                "post.pipeline-layout") \
    X(POST_PIPELINE,              "post.pipeline") \
    X(POST_BIND_GROUP,            "post.bind-group") \
    X(POST_PASS,                  "post.pass") \
    X(SURFACE_DIRECT_VIEW,        "surface.direct-view") \
    X(FRAME_DUMP_BUFFER,          "capture.frame-buffer") \
    X(SURFACE_BLIT_VIEW,          "surface.blit-view") \
    X(OVERLAY_VIEW,               "overlay.view") \
    X(OVERLAY_PASS,               "overlay.pass") \
    X(SURFACE_DUMP_BUFFER,        "capture.surface-buffer") \
    X(SHADER_BGL,                 "shader.bind-group-layout") \
    X(SHADER_MODULE,              "shader.module") \
    X(SHADER_LAYOUT,              "shader.pipeline-layout") \
    X(PIPELINE_ASYNC,             "shader.pipeline-async") \
    X(PIPELINE_SYNC,              "shader.pipeline-sync") \
    X(PIPELINE_PREWARM,           "shader.pipeline-prewarm") \
    X(TEXTURE,                    "texture.texture") \
    X(TEXTURE_VIEW,               "texture.view") \
    X(MIP_TEXTURE,                "texture.mip-texture") \
    X(MIP_VIEW,                   "texture.mip-view") \
    X(MATERIAL_SAMPLER,           "texture.sampler") \
    X(WHITE_TEXTURE,              "draw.white-texture") \
    X(WHITE_VIEW,                 "draw.white-view") \
    X(DEFAULT_SAMPLER,            "draw.default-sampler") \
    X(VERTEX_BUFFER,              "draw.vertex-buffer") \
    X(SNAPSHOT_TEXTURE,           "memory-blend.texture") \
    X(SNAPSHOT_VIEW,              "memory-blend.view") \
    X(SNAPSHOT_SAMPLER,           "memory-blend.sampler") \
    X(SNAPSHOT_PASS,              "memory-blend.pass") \
    X(DIAG_UNIFORM,               "draw.diagnostic-uniform") \
    X(DRAW_BIND_GROUP,            "draw.bind-group") \
    X(MINIMAP_MODULE,             "minimap.module") \
    X(MINIMAP_BGL,                "minimap.bind-group-layout") \
    X(MINIMAP_UNIFORM,            "minimap.uniform") \
    X(MINIMAP_BIND_GROUP,         "minimap.bind-group") \
    X(MINIMAP_LAYOUT,             "minimap.pipeline-layout") \
    X(MINIMAP_PIPELINE,           "minimap.pipeline") \
    X(MINIMAP_PASS,               "minimap.pass") \
    X(PARTIAL_FINISH,             "readback.partial-finish") \
    X(PARTIAL_ENCODER,            "readback.partial-encoder") \
    X(PARTIAL_PASS,               "readback.partial-pass") \
    X(READBACK_RESOLVE_ENCODER,   "readback.resolve-encoder") \
    X(READBACK_RESOLVE_FINISH,    "readback.resolve-finish") \
    X(READBACK_BUFFER,            "readback.buffer") \
    X(READBACK_ENCODER,           "readback.encoder") \
    X(READBACK_FINISH,            "readback.finish") \
    X(READBACK_MAP,               "readback.map") \
    X(MODERN_MODULE,              "modern.module") \
    X(MODERN_BGL,                 "modern.bind-group-layout") \
    X(MODERN_LAYOUT,              "modern.pipeline-layout") \
    X(MODERN_PIPELINE,            "modern.pipeline") \
    X(MODERN_VERTEX_BUFFER,       "modern.vertex-buffer") \
    X(MODERN_INDEX_BUFFER,        "modern.index-buffer") \
    X(MODERN_TEXTURE,             "modern.texture") \
    X(MODERN_VIEW,                "modern.view") \
    X(MODERN_UNIFORM,             "modern.uniform") \
    X(MODERN_SAMPLER,             "modern.sampler") \
    X(MODERN_BIND_GROUP,          "modern.bind-group")

enum GfxWebgpuFaultPoint {
#define GFX_WEBGPU_FAULT_ENUM(symbol, name) GFX_WEBGPU_FAULT_##symbol,
    GFX_WEBGPU_FAULT_POINT_LIST(GFX_WEBGPU_FAULT_ENUM)
#undef GFX_WEBGPU_FAULT_ENUM
    GFX_WEBGPU_FAULT_COUNT
};

/* Parse MDKR_WEBGPU_FAULT. False means a non-empty malformed/unknown spec. */
bool gfx_webgpu_fault_configure(void);

/* Count a visit and return true only for the configured point/occurrence. */
bool gfx_webgpu_fault_hit(enum GfxWebgpuFaultPoint point);

/* True when point is selected, without consuming an occurrence. This lets a
 * test route execution to a normally conditional fallback before injecting
 * inside that fallback. */
bool gfx_webgpu_fault_selected(enum GfxWebgpuFaultPoint point);

size_t gfx_webgpu_fault_point_count(void);
const char *gfx_webgpu_fault_point_name(size_t index);
const char *gfx_webgpu_fault_error(void);

/* ROM/GPU-free unit-test seam. NULL restores environment-driven configuration. */
void gfx_webgpu_fault_set_spec_for_test(const char *spec);
void gfx_webgpu_fault_reset_for_test(void);

#endif
