/*
 * gfx_webgpu_shader.h — WGSL combiner-shader emitter for the WebGPU backend.
 *
 * Emits a single WGSL module (a @vertex vs_main + a @fragment fs_main) from the
 * N64 color-combiner id (shader_id0/shader_id1), mirroring the GLSL emitter in
 * gfx_opengl.c (gfx_opengl_create_and_load_new_shader) and the MSL port in
 * gfx_metal.mm. The vertex-attribute layout is the same deterministic CCFeatures
 * walk both of those use, returned here so gfx_webgpu.c can build the matching
 * WGPUVertexBufferLayout. Compiled only under MDKR_WEBGPU_BACKEND.
 */
#ifndef GFX_WEBGPU_SHADER_H
#define GFX_WEBGPU_SHADER_H

#include <stdbool.h>
#include <stdint.h>

/* One interleaved vertex attribute in the buf_vbo stream: WGSL @location, its
 * component count (1..4 floats), and its float offset within the vertex. */
struct WgpuVtxAttr {
    int location;
    int size;     /* 1, 2, 3, or 4 floats */
    int offset;   /* in floats from the start of the vertex */
};

/*
 * CPU-side shader metadata and WebGPUVertexAttribute storage must always have
 * the same bound. Keep this above WebGPU's guaranteed 16-attribute floor so
 * the emitter can describe a layout before the adapter-limit gate rejects it.
 */
#define MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY 32

/* Everything gfx_webgpu.c needs to build the pipeline + bind groups for a
 * combiner, alongside the emitted WGSL. */
struct WgpuShaderInfo {
    struct WgpuVtxAttr attrs[MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY];
    int  num_attrs;
    int  num_floats;          /* vertex stride, in floats */
    int  num_inputs;          /* CCFeatures.num_inputs (shade/prim/env slots) */
    bool used_textures[2];
    bool opt_alpha;
    bool opt_texture_edge;    /* alpha-cutout: discard below the RDP edge threshold */
    bool opt_dfdx_light;      /* RL-5 smooth-normal, linear-light directional sun */
    bool opt_sun_shadow;      /* world-space cascaded directional-shadow receiver */
    /* WEB-027: this combiner reads SHADER_NOISE, so its pipeline gets ONE extra
     * uniform (group 0, @binding(7): the per-frame noise counter). Set only for
     * noise-using combiners so noise-free pipelines keep their exact bindings. */
    bool uses_noise;
    /* RDP memory-blend emulation (glass / chain-link fence class — room XLU
     * wrap-color-on-coverage surfaces, gfx_pc's default for that raw mode).
     * The fragment samples a snapshot of the scene target ("memory color") and
     * performs the N64 blender byte math in-shader, exactly like the GLSL arm
     * in gfx_opengl.c (:1373/:1401). Bindings 4/5 = snapshot texture/sampler;
     * the cvg variant additionally needs the GL-convention viewport via the
     * @binding(6) uniform. HW blending stays disabled for these (shader blends). */
    bool diag_rdp_memory_blend;      /* color-only memory blend (diag CC list) */
    bool diag_rdp_cvg_memory_blend;  /* coverage-wrap memory blend (default)   */
};

/* Build the WGSL for a combiner id. Returns a heap-allocated NUL-terminated
 * module (caller frees) and fills *info, or NULL on allocation failure. */
char *gfx_webgpu_build_wgsl(uint64_t shader_id0, uint32_t shader_id1,
                            struct WgpuShaderInfo *info);

/* Static WGSL for the output-VI-filter post-FX pass (fullscreen triangle). A
 * faithful port of gfx_opengl.c's output-filter fragment shader (FXAA / bloom /
 * color-grade / filmic tonemap / gamma / vignette / CAS sharpen / Bayer dither /
 * RGB555). Runs between the offscreen scene resolve and the surface present in
 * gfx_webgpu.c; SSAO is intentionally omitted (needs a sampleable depth target,
 * default-off — see WEBGPU_BACKEND_STATUS). Returned as a NUL-terminated string
 * literal (no allocation). */
const char *gfx_webgpu_postfx_wgsl(void);

#endif /* GFX_WEBGPU_SHADER_H */
