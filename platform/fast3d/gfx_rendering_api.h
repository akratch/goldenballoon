/**
 * gfx_rendering_api.h — Rendering backend vtable.
 * From Emill/n64-fast3d-engine (n64-fast3d-engine license — modified
 * BSD-2-Clause; see src/platform/fast3d/PROVENANCE.md), unmodified.
 */
#ifndef GFX_RENDERING_API_H
#define GFX_RENDERING_API_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct ShaderProgram;

/* Blend modes for set_blend_mode() */
enum GfxBlendMode {
    GFX_BLEND_DISABLED = 0,  /* opaque — no blending */
    GFX_BLEND_ALPHA    = 1,  /* standard alpha: src*srcA + dst*(1-srcA) */
    GFX_BLEND_MODULATE = 2,  /* multiplicative: src*dst (darkening/shadows) */
    GFX_BLEND_ALPHA_COVERAGE = 3, /* alpha blend plus sample coverage */
    GFX_BLEND_ALPHA_CVG_WRAP_STENCIL = 4, /* stencil coverage wrap */
    GFX_BLEND_ALPHA_RDP_MEMORY = 5, /* shader samples memory color */
    GFX_BLEND_ALPHA_RDP_CVG_MEMORY = 6, /* shader coverage + memory color */
};

enum GfxRenderingStatus {
    GFX_RENDERING_UNINITIALIZED = 0,
    GFX_RENDERING_READY,
    GFX_RENDERING_TRANSIENT,
    GFX_RENDERING_FATAL
};

/* Modern-mesh draw path (W9 scene decoration): a full-fidelity mesh drawn by
 * the backend itself — float32 vertices, u32 indices, mipmapped RGBA8 texture
 * of arbitrary size — into the SAME color/depth attachments as the N64 scene,
 * transformed on the GPU by the interpreter's current MP matrix. Vertex
 * layout is interleaved, stride 36: pos float3 @0, normal float3 @12,
 * uv float2 @24, rgba u8x4 (normalized) @32. `backend_handle` starts NULL;
 * the backend uploads GPU resources on first draw and caches them there. */
struct GfxModernMesh {
    uint32_t mesh_id;      /* loader-assigned, monotonic, never reused --
                              the backend caches GPU resources by this id
                              (struct addresses recycle across level loads) */
    const float *vtx;      /* vtx_count * 9 floats (36-byte stride) */
    uint32_t vtx_count;
    const uint32_t *idx;
    uint32_t idx_count;
    const uint8_t *tex_rgba; /* tex_w * tex_h * 4 */
    int tex_w, tex_h;
    int cutout;            /* alpha-cutout (discard), drawn two-sided */
    void *backend_handle;
};

struct GfxRenderingAPI {
    bool (*z_is_from_0_to_1)(void);
    void (*unload_shader)(struct ShaderProgram *old_prg);
    void (*load_shader)(struct ShaderProgram *new_prg);
    struct ShaderProgram *(*create_and_load_new_shader)(uint64_t shader_id0, uint32_t shader_id1);
    struct ShaderProgram *(*lookup_shader)(uint64_t shader_id0, uint32_t shader_id1);
    void (*shader_get_info)(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]);
    uint32_t (*new_texture)(void);
    void (*delete_texture)(uint32_t texture_id);
    void (*select_texture)(int tile, uint32_t texture_id);
    bool (*upload_texture)(const uint8_t *rgba32_buf, int width, int height);
    void (*set_sampler_parameters)(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt);
    void (*set_depth_mode)(bool depth_test, bool depth_update, bool depth_compare,
                           bool depth_source_prim, uint16_t zmode);
    void (*set_viewport)(int x, int y, int width, int height);
    void (*set_scissor)(int x, int y, int width, int height);
    /* Optional per-world-viewport shadow receiver selection. */
    void (*set_shadow_view)(int view_index);
    void (*set_blend_mode)(enum GfxBlendMode mode);
    void (*draw_triangles)(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
    bool (*read_framebuffer_rgb)(int x, int y, int width, int height, uint8_t *rgb_out);
    /* Return false when the backend cannot create a usable device/context.
     * Startup must never enter game code with an inert renderer. */
    bool (*init)(void);
    void (*on_resize)(void);
    /* Begin one backend frame transaction. False means no encoder/context was
     * opened, so the caller must not walk or end the frame. */
    bool (*start_frame)(void);
    /*
     * Complete the render-resolution scene and begin a load-preserving pass at
     * the physical output resolution. Optional: a NULL backend keeps the
     * historical single-target path. The frontend calls this at most once per
     * frame, immediately before the first authored SAFE_2D/FULLBLEED draw.
     */
    bool (*begin_output_overlay)(void);
    void (*end_frame)(void);
    void (*finish_render)(void);
    /* OPTIONAL (NULL on backends without it — call sites must guard, like
     * read_framebuffer_rgb): draw a modern mesh at the current DL position.
     * mvp = the interpreter's current MP matrix (row-vector convention);
     * fog_* mirror the N64 per-vertex fog curve (see gfx_pc.c fog math). */
    void (*draw_modern_mesh)(struct GfxModernMesh *mesh, const float mvp[4][4],
                             const float fog_color[3], float fog_mul,
                             float fog_offset, int fog_enabled);
    /* OPTIONAL (NULL when the backend has no mip support — call sites must
     * guard). Uploads a complete mip chain built by platform/fast3d/gfx_mipgen.c. */
    bool (*upload_texture_mipped)(const uint8_t *const *level_rgba,
                                  const int *level_w, const int *level_h,
                                  int level_count);
    /*
     * Final process/runtime teardown. This is distinct from device recovery:
     * it releases every backend-owned child and CPU allocation while the
     * native graphics host is still alive. Borrowed shell roots must not be
     * released by the backend.
     */
    void (*shutdown)(void);
    /*
     * Runtime lifecycle health. NULL means the backend has no asynchronous
     * failure source and remains usable after successful init. A FATAL result
     * is consumed at the scheduler boundary so game state cannot keep advancing
     * behind an inert renderer.
     */
    enum GfxRenderingStatus (*get_status)(void);
};

#endif
