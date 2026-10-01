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

#include "modern_character_capture_projection.h"
#include "modern_character_gpu_timing.h"

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

/* Game-owned draw scopes used only by the Character Workshop's one-shot
 * attribution witness. Values are a stable renderer contract: the display-list
 * interpreter flushes ordinary triangles before changing scope, so a backend
 * can retain exact batches without guessing from textures or mesh names. */
enum GfxModernCharacterOccluder {
    GFX_MODERN_CHARACTER_OCCLUDER_NONE = 0,
    GFX_MODERN_CHARACTER_OCCLUDER_VEHICLE_BODY,
    GFX_MODERN_CHARACTER_OCCLUDER_VEHICLE_PARTS,
    GFX_MODERN_CHARACTER_OCCLUDER_HELD_OBJECT,
    GFX_MODERN_CHARACTER_OCCLUDER_COUNT,
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
    uint32_t texture_id;   /* immutable shared atlas identity; zero retains the
                             legacy per-mesh texture ownership contract */
    uint32_t mesh_id;      /* loader-assigned, monotonic, never reused --
                              the backend caches GPU resources by this id
                              (struct addresses recycle across level loads) */
    uint32_t geometry_id;  /* immutable shared vertex/index identity; zero
                              keeps geometry private to this mesh_id */
    const float *vtx;      /* vtx_count * 9 floats (36-byte stride) */
    uint32_t vtx_count;
    const uint32_t *idx;
    uint32_t idx_count;
    const uint8_t *tex_rgba; /* tex_w * tex_h * 4 */
    int tex_w, tex_h;
    int cutout;            /* alpha-cutout (discard), drawn two-sided */
    void *backend_handle;
};

/* The replaced sprite's material state for a modern mesh draw: tint is the
 * prim colour (rgb modulate, alpha fade); env is the colour the billboard's
 * second combiner cycle lerps toward by env[3] (zero when it has none). */
struct GfxModernMeshMaterial {
    float tint[4];
    float env[4];
};

/* Generic high-fidelity character resources. These are immutable after
 * publication; a backend caches uploads by asset_id and release_modern_asset()
 * retires them before the CPU owner frees any bytes. */
struct GfxModernSkinnedVertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv[2];
    uint16_t joints[4];
    float weights[4];
};

struct GfxModernTexture {
    const uint8_t *level_rgba[13];
    int level_width[13];
    int level_height[13];
    int level_count;
    int wrap_s;
    int wrap_t;
    int min_filter;
    int mag_filter;
    /* A BasisU KTX2 stays compressed in the immutable MDKC mapping until the
     * active backend chooses a device-native block format. PNGs leave these
     * fields zero and retain the decoded level_rgba path above. */
    const uint8_t *ktx2_data;
    size_t ktx2_size;
    uint32_t ktx2_flags; /* material role: sRGB colour, linear data, or normal */
};

struct GfxModernMaterial {
    int texture[5]; /* base, metallic/roughness, normal, occlusion, emissive */
    float base_color[4];
    float emissive[3];
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    float alpha_cutoff;
    uint32_t flags; /* bits 0..1 alpha mode, bit 2 double-sided */
};

struct GfxModernPrimitive {
    uint32_t first_index;
    uint32_t index_count;
    uint32_t material;
    uint32_t node;
    int32_t skin;
    uint32_t lod;
};

struct GfxModernSkinnedAsset {
    uint64_t asset_id;
    const struct GfxModernSkinnedVertex *vertices;
    uint32_t vertex_count;
    const uint32_t *indices;
    uint32_t index_count;
    const struct GfxModernPrimitive *primitives;
    uint32_t primitive_count;
    const struct GfxModernMaterial *materials;
    uint32_t material_count;
    const struct GfxModernTexture *textures;
    uint32_t texture_count;
};

struct GfxModernSkinnedDraw {
    const struct GfxModernSkinnedAsset *asset;
    uint32_t primitive;
    /* Local-player ownership and the donor-target frame are retained with each
     * command so an isolated Workshop capture can select one subject and
     * compose exact target coordinates with the camera MVP. */
    uint32_t player;
    uint32_t view;
    /* Workshop donor comparison prepares the exact custom pose/projection but
     * suppresses its pixels so the ordinary retail donor remains visible. */
    uint32_t reference_only;
    float target_frame_matrix[16];
    uint32_t capture_bounds_valid;
    float capture_bounds_min[3];
    float capture_bounds_max[3];
    /* HLE-owned immutable shadow binding for this authored draw. The matrix
     * maps donor-object coordinates to world space (column-major), allowing
     * every visible material to receive while shadow_cast_valid separately
     * admits only qualified opaque/masked primitives as casters. */
    uint32_t shadow_binding_valid;
    uint32_t shadow_cast_valid;
    /* Current authored caster-frame view. Receiving deliberately uses the
     * backend's separately resolved previous-frame view. */
    uint32_t shadow_cast_view;
    float shadow_world_matrix[16];
    /* Primitive-local glTF node transform, including the package's authored
     * presentation transform. Column-major, applied after skinning and before
     * the display-list object's MVP. */
    float model_matrix[16];
    float normal_matrix[16]; /* inverse-transpose(model), column-major */
    const float *bone_matrices; /* bone_count column-major mat4 values */
    /* Immutable previous authored-tick endpoints. Presentation replay blends
     * these toward the current fields with the same rational alpha used for
     * the donor object, without following mutable runtime pose storage. */
    float previous_model_matrix[16];
    const float *previous_bone_matrices;
    uint32_t bone_count;
    /* Exact presentation-camera eye transformed into donor-object space by
     * the HLE walk. The shader uses this for view-dependent material response;
     * an unavailable/cut-incomplete camera retains the bounded legacy view. */
    uint32_t camera_position_valid;
    float camera_position[3];
    float light_direction[3];   /* normalized in asset/model space */
    float ambient;
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
    /* Optional exact Workshop attribution scope. NULL backends still render
     * normally; callers must flush before changing it. */
    void (*set_modern_character_occluder)(uint32_t occluder);
    void (*draw_triangles)(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
    bool (*read_framebuffer_rgb)(int x, int y, int width, int height, uint8_t *rgb_out);
    /* Optional exact modern-character-only capture. The backend replays its
     * validated character commands into an isolated transparent target and
     * returns bottom-left-origin straight RGBA. */
    bool (*get_modern_character_capture_dimensions)(uint32_t *width,
                                                     uint32_t *height);
    bool (*get_modern_character_capture_projection)(
        MdkrModernCharacterCaptureProjection *projection);
    /* Exact projection retained from player one's accepted scene draw. This
     * is cheap matrix/viewport evidence only; it does not read pixels or walk
     * the skinned mesh and therefore does not perturb performance tests. */
    bool (*get_modern_character_scene_projection)(
        MdkrModernCharacterCaptureProjection *projection);
    bool (*read_modern_character_capture_rgba)(int width, int height,
                                                uint8_t *rgba_out);
    /* Optional exact GPU timestamp evidence for the Workshop. begin() resets
     * after warm-up; finish() stops admission and never waits for readback. */
    void (*begin_modern_character_gpu_timing)(void);
    void (*finish_modern_character_gpu_timing)(
        MdkrModernCharacterGpuTimingMetrics *out);
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
    bool (*draw_modern_mesh)(struct GfxModernMesh *mesh, const float mvp[4][4],
                             const float fog_color[3], float fog_mul,
                             float fog_offset, int fog_enabled,
                             const struct GfxModernMeshMaterial *material);
    /* OPTIONAL: false once the backend switched its optional modern-mesh
     * path off after an allocation failure; authoring then keeps the
     * original sprite. NULL means always available when draw_modern_mesh is. */
    bool (*modern_mesh_available)(void);
    void (*draw_modern_skinned)(const struct GfxModernSkinnedDraw *draw,
                                const float mvp[4][4],
                                const float fog_color[3], float fog_mul,
                                float fog_offset, int fog_enabled);
    void (*release_modern_asset)(uint64_t asset_id);
    /* Optional static-mesh cache retirement before pack CPU storage is freed.
     * Does not retire skinned-character resources. */
    void (*clear_modern_mesh_cache)(void);
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
