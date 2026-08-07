/*
 * gfx_webgpu.c — Cross-platform WebGPU (wgpu-native) render backend.
 *
 * Implements the Fast3D `GfxRenderingAPI` vtable (gfx_rendering_api.h) — the
 * same ~23-function seam gfx_opengl.c and gfx_metal.mm fill — against the
 * standard webgpu.h C API. Selected at runtime by the renderer environment
 * variable; compiled and linked only when MDKR_WEBGPU_BACKEND (see
 * cmake/webgpu.cmake). The same translation unit serves native (wgpu-native)
 * and browser (emdawnwebgpu) builds; every dialect difference is confined to
 * gfx_webgpu_compat.h.
 *
 * Invariants this file holds:
 *   - The whole vtable is filled: a caller may invoke any GfxRenderingAPI entry
 *     point in any order the frontend produces, and no entry point may crash on
 *     a partially initialized or already-lost device.
 *   - The scene is rendered into an offscreen color/depth pair and copied to the
 *     surface at present, so the surface's format and usages never constrain the
 *     draw path, and readback always has a stable source texture.
 *   - Surface loss/outdate/timeout is TRANSIENT and retried; device, queue,
 *     validation and allocation failures latch a FATAL state that stops
 *     simulation at the next scheduler boundary rather than drawing garbage.
 *   - GPU objects owned by an embedding app shell are adopted, never released
 *     here; objects this file creates are released exactly once at shutdown.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx_webgpu_compat.h"   /* dialect seam: webgpu.h/wgpu.h, pump, waits, surface */

#include "gfx_mipgen.h"        /* mdkr64: g_pcMipmaps, g_gfxSamplerLod0Only */
#include "gfx_cc.h"            /* shader option bits for receiver prewarm twins */
#include "gfx_render_scale.h"   /* mdkr64: gfx_render_scaled_dimension */
#include "gfx_rendering_api.h"
#include "gfx_shadow_cascade.h"
#include "gfx_shadow_frame.h"
#include "fs_utf8.h"
#include "present_sched.h"

/* platform/fast3d/gfx_pc_dkr.c — declared rather than included: gfx_pc_dkr.h
 * pulls in the F3DDKR/gbi headers, which redefine this file's GfxDimensions. */
bool gfx_dkr_replay_pass_active(void);

/* Resolves a bare filename to a host-writable location (platform-owned; see
 * platform/gfx_webgpu_stubs.c). Used for the pipeline-prewarm cache and the
 * debug frame dumps, both of which must not assume a POSIX temp directory. */
extern const char *savedirPath(const char *filename);

#include "gfx_webgpu.h"          /* public surface helper (shared with AppHost) */
#include "gfx_webgpu_callback_latch.h"
#include "gfx_webgpu_async_request.h"
#include "gfx_webgpu_surface_policy.h"
#include "gfx_webgpu_fault.h"    /* deterministic lifecycle/allocation faults */
#include "gfx_webgpu_shader.h"   /* WGSL combiner emitter */
#include "gfx_uniforms.h"        /* g_pc* render/post-FX uniform state (shared w/ GL/Metal) */
#include "platform_os.h"         /* typed SDL native-window/surface seam */

/* Route fallible object constructors through the stable fault vocabulary.
 * The ROM/GPU-free registry test prevents point-name drift; the remaining
 * runtime matrix is tracked as an explicit foundation-completion gate. */
#define WGPU_FAULT_CREATE(point, expression) \
    (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_##point) ? NULL : (expression))

/* gfx_current_dimensions is the frontend render resolution the viewports and
 * T&L are computed against (gfx_pc.c). Declared here exactly as gfx_metal.mm
 * does — the shared header is C++-guarded, so each backend re-declares the POD
 * view it needs. Read every frame to size the swapchain. */
struct GfxDimensions { uint32_t width, height; float aspect_ratio; };
extern struct GfxDimensions gfx_current_dimensions;
extern struct GfxDimensions gfx_output_dimensions;   /* mdkr64: pre-RenderScale output size */

/* ------------------------------------------------------------------------
 * Backend state
 * ---------------------------------------------------------------------- */
static WGPUInstance      s_instance = NULL;
static WGPUAdapter       s_adapter  = NULL;
static WGPUDevice        s_device   = NULL;
static WGPUQueue         s_queue    = NULL;
static WGPUSurface       s_surface  = NULL;
static WGPUTextureFormat s_surface_format = WGPUTextureFormat_Undefined;
static bool              s_ready    = false;   /* device + surface both live */
/*
 * Runtime health is observable through the rendering API. TRANSIENT means a
 * surface timeout/outdate/loss is being retried; FATAL is latched by device,
 * queue, validation, or allocation failure and stops simulation at the next
 * scheduler boundary.
 */
static enum GfxRenderingStatus s_runtime_status =
    GFX_RENDERING_UNINITIALIZED;
#define WGPU_SURFACE_RECOVERY_LIMIT 120u
static unsigned s_surface_recovery_attempts = 0;
static bool s_native_recovery_attempted = false;
static bool s_callback_recovery_pending = false;
static uintptr_t s_next_device_generation = 0;
/* Device callbacks are registered once in WGPUDeviceDescriptor and remain
 * attached for the complete device lifetime, including AppHost -> engine ->
 * AppHost borrowing. Work callbacks instead carry raw engine-session state and
 * must be invalidated as soon as that session shuts down. */
static uintptr_t s_active_work_generation = 0;
static WGPUDevice s_callback_device = NULL;

/* PAC-005: bound WebGPU work wherever the host can produce images faster than
 * the GPU retires them. Runtime admission only polls: it never drains or waits
 * on the cooperative game/audio thread. A full queue therefore holds the last
 * complete image while simulation continues. Orderly shutdown may still drain
 * after gameplay service has stopped. Presentation replay uses the stricter
 * one-frame admission limit so the second slot remains reserved for the next
 * authored endpoint. */
#define WGPU_FRAME_IN_FLIGHT_MAX 2u
static unsigned s_gpu_frames_in_flight;
static unsigned s_gpu_frames_in_flight_high_water;
static uint64_t s_gpu_frame_submissions;
static uint64_t s_gpu_frame_completions;
static uint64_t s_gpu_surface_presents;
static uint64_t s_gpu_surface_holds;
static uint64_t s_gpu_surface_unavailable;
static uint64_t s_gpu_backpressure_waits;
static uint64_t s_gpu_backpressure_polls;
static uint64_t s_gpu_backpressure_skips;
static uint64_t s_gpu_endpoint_admission_skips;
static uint64_t s_gpu_replay_admission_skips;
static uint64_t s_gpu_completion_failures;
static uint64_t s_gpu_abandoned_completions;
static uint64_t s_gpu_backpressure_wait_ns;
static uint64_t s_gpu_runtime_waits;
static uint64_t s_gpu_runtime_wait_ns;
static uint64_t s_gpu_first_submit_ns;
static uint64_t s_gpu_last_submit_ns;
/* When the app shell owns the device/surface (launcher → game handoff), the
 * engine adopts them and must NOT release them at teardown. False = we created
 * them ourselves (standalone --level boot) and own their lifetime. */
static bool              s_owns_device = false;
/* WEB-015: the maxTextureDimension2D actually granted to the device. WebGPU's
 * DEFAULT device limit is 8192, but most desktop GPUs support 16384; bring-up
 * requests the adapter's real max and records the granted value here so the
 * offscreen-dim query and the upload-size reject both reflect the true cap
 * (not the 8192 default that clamped RenderScale 2 into resample blur). Stays
 * at the guaranteed 8192 floor when bring-up is skipped (host-adopted device). */
static uint32_t          s_max_tex_dim = 8192;

/* Host WebGPU handoff (src/platform/host_window.c). Present only when the app
 * shell created a device/queue/surface and registered them before boot. */
extern int   platformHasHostWebGpu(void);
extern void *platformHostWgpuInstance(void);
extern void *platformHostWgpuAdapter(void);
extern void *platformHostWgpuDevice(void);
extern void *platformHostWgpuQueue(void);
extern void *platformHostWgpuSurface(void);
extern int   platformHostWgpuSurfaceFormat(void);
extern int   platformHasHostWebGpuRecovery(void);
extern int   platformRecoverHostWebGpu(int phase);
enum {
    HOST_WEBGPU_RECOVERY_PREPARE = 1,
    HOST_WEBGPU_RECOVERY_COMMIT = 2,
    HOST_WEBGPU_RECOVERY_ABORT = 3,
};

/* Configured swapchain size; 0 forces a (re)configure on the next start_frame. */
static uint32_t s_cfg_w = 0, s_cfg_h = 0;
/*
 * A reconfigure owed for a reason the SIZE cannot express -- today, a display
 * refresh change that invalidates the present-mode ranking baked at the last
 * configuration (gfx_webgpu_request_surface_reconfigure). Consumed by the next
 * configuration attempt, successful or not: a surface that cannot be
 * configured is already a fatal path, and retrying it every frame would only
 * bury that in noise.
 */
static bool s_cfg_present_mode_dirty = false;
static bool s_surface_copy_dst = false;
static bool s_surface_copy_src = false;

/* PERF-020: resize debounce state. The last requested (not-yet-committed) size and
 * how many consecutive frames it has been held; a drag is only applied once the size
 * settles for WGPU_RESIZE_STABLE_FRAMES frames (see wgpu_start_frame). */
static uint32_t s_resize_pending_w = 0, s_resize_pending_h = 0;
static int      s_resize_stable = 0;

/* Offscreen scene target: the game renders here (not straight to the surface),
 * so rendering is independent of window visibility (a hidden/occluded window has
 * no drawable) and the frame can be read back for screenshots. BGRA8 to
 * match the surface so end_frame presents with a plain texture-to-texture copy.
 * Re-created when the render resolution changes. */
/* mdkr64 supersample resolve — output-sized target the present copy, readback
 * and the frame dumper all read. See wgpu_run_resolve(). */
static WGPURenderPipeline    s_resolve_pipe = NULL;
static WGPUBindGroupLayout   s_resolve_bgl  = NULL;
static WGPUBuffer            s_resolve_ubuf = NULL;
/*
 * Per-frame ring of resolve uniform buffers, for the same reason the diag
 * viewport ring exists: wgpuQueueWriteBuffer executes BEFORE the command
 * buffer, so a frame that resolves more than once (supersample resolve, then
 * the present blit) would run every pass with the LAST pass's src/dst extents
 * and tap count. Slot 0 is s_resolve_ubuf, so a single-resolve frame allocates
 * nothing extra; slots grow on demand and are reused across frames (only the
 * used count resets).
 */
static WGPUBuffer *s_resolve_ubo_ext = NULL;
static int         s_resolve_ubo_ext_cap = 0;
static int         s_resolve_ubo_used = 0;
static WGPUSampler           s_resolve_samp = NULL;
static WGPUTexture           s_resolve_tex  = NULL;
static WGPUTextureView       s_resolve_view = NULL;
static uint32_t              s_resolve_w = 0, s_resolve_h = 0;
/* Output-resolution depth attachment used only after the frontend crosses the
 * scene -> SAFE_2D/FULLBLEED boundary. Existing Fast3D pipelines include a
 * depth-stencil state, so the output pass must provide a compatible attachment
 * even though authored HUD materials normally disable depth testing. */
static WGPUTexture           s_output_depth_tex  = NULL;
static WGPUTextureView       s_output_depth_view = NULL;

static WGPUTexture           s_scene_tex  = NULL;
static WGPUTextureView       s_scene_view = NULL;
static WGPUTexture           s_depth_tex  = NULL;
static WGPUTextureView       s_depth_view = NULL;
static uint32_t              s_scene_w = 0, s_scene_h = 0;
#define WGPU_DEPTH_FORMAT WGPUTextureFormat_Depth24Plus

/* Output post-FX target: the scene (s_scene_tex) is resolved through the
 * fullscreen output-VI-filter pass into here, and THIS is what gets presented /
 * read back / has the minimap drawn on top — mirroring GL's default-FB composite
 * and Metal's s_final_color. Only allocated/used when the filter is active; a
 * faithful (RemasterFX-off, gamma 1.0) frame keeps the plain scene->surface copy.
 * Same BGRA8 format + size as the scene target. */
static WGPUTexture           s_post_tex   = NULL;
static WGPUTextureView       s_post_view  = NULL;
/* The target the minimap overlay + the present copy + readback read from this
 * frame: s_post_view when the filter ran, else s_scene_view. Set in end_frame. */
static WGPUTextureView       s_present_target_view = NULL;
static WGPUTexture           s_present_target_tex  = NULL;

/* PERF-008: sticky "a frame is or may be read back" latch. Set once at startup for the
 * known env/flag-armed capture paths (see wgpu_readback_possible), and — as a universal
 * safety net — the first time wgpu_read_framebuffer_rgb actually runs, so ANY readback
 * caller (the gfx_pc.c diagnostic pixel probes, or a future one) pins every subsequent
 * frame to the offscreen present path even if its arming signal was not enumerated. */
static bool                  s_readback_latched    = false;

/* Per-frame objects, valid only between start_frame and end_frame. */
static WGPUCommandEncoder    s_encoder    = NULL;
/*
 * WEB-053: identifies the command encoder a recorded draw belongs to. Bumped
 * once per encoder creation; a texture entry stamps it when a draw that samples
 * the entry is recorded. Comparing the stamp against the live epoch answers the
 * only question the in-place texture re-upload needs: "could an already recorded
 * but not yet submitted draw still read these texels?"
 */
static uint32_t              s_draw_epoch = 1;
static WGPURenderPassEncoder s_pass       = NULL;
static bool                  s_frame_open = false;
static bool                  s_output_overlay_active = false;

static uint32_t wgpu_draw_target_w(void) {
    return s_output_overlay_active ? s_resolve_w : s_scene_w;
}

static uint32_t wgpu_draw_target_h(void) {
    return s_output_overlay_active ? s_resolve_h : s_scene_h;
}

static WGPUTexture wgpu_draw_target_tex(void) {
    return s_output_overlay_active ? s_resolve_tex : s_scene_tex;
}

static WGPUTextureView wgpu_draw_target_view(void) {
    return s_output_overlay_active ? s_resolve_view : s_scene_view;
}

static WGPUTextureView wgpu_draw_depth_view(void) {
    return s_output_overlay_active ? s_output_depth_view : s_depth_view;
}

/* The app-shell F1 overlay renders into this render pass on the surface texture,
 * opened by wgpu_end_frame just before present. Non-NULL only during the
 * platformOverlayRender() call; the overlay reads it via the getters below. */
static WGPURenderPassEncoder s_overlay_pass = NULL;
static int s_overlay_w = 0, s_overlay_h = 0;
static bool s_overlay_failure_reported = false;

/* Exposed to the app shell's F1 overlay (ui_overlay.cpp) so it can draw ImGui
 * into the current surface pass via gfx_webgpu_imgui_render. NULL when no overlay
 * pass is open. Declared in gfx_webgpu.h. */
void *gfx_webgpu_current_overlay_pass(void) { return (void *)s_overlay_pass; }
void  gfx_webgpu_current_overlay_size(int *w, int *h) {
    if (w) *w = s_overlay_w;
    if (h) *h = s_overlay_h;
}

/* RDP memory-blend ("glass / chain-link fence" class) snapshot resources.
 * Before each draw whose shader samples the memory color, the open scene pass
 * is split and the scene target is copied here — the WGSL then reads it as the
 * N64 "memory color" (the WebGPU equivalent of gfx_opengl.c's per-batch
 * glCopyTexSubImage2D snapshot, W3.6 fence/glass regression fix). */
static WGPUTexture     s_snap_tex  = NULL;
static WGPUTextureView s_snap_view = NULL;
static uint32_t        s_snap_w = 0, s_snap_h = 0;
static WGPUTexture     s_output_snap_tex  = NULL;
static WGPUTextureView s_output_snap_view = NULL;
static uint32_t        s_output_snap_w = 0, s_output_snap_h = 0;
static WGPUSampler     s_snap_sampler = NULL;   /* nearest + clamp-to-edge */

static WGPUTextureView wgpu_draw_snapshot_view(void) {
    return s_output_overlay_active ? s_output_snap_view : s_snap_view;
}
/* Small per-frame ring of 16-byte viewport UBOs for the coverage-wrap shader
 * (GL uDiagViewport). Distinct buffers per distinct value are required because
 * wgpuQueueWriteBuffer executes before the command buffer — one buffer would
 * retroactively apply the LAST viewport to every draw. In practice the scene
 * viewport is constant within a frame, so slot 0 is reused. */
#define WGPU_DIAG_UBO_RING 8
static WGPUBuffer s_diag_ubo[WGPU_DIAG_UBO_RING];
static float      s_diag_ubo_val[WGPU_DIAG_UBO_RING][4];
static int        s_diag_ubo_used = 0;   /* slots written this frame (ring + overflow) */
/* WEB-051: overflow buffers for the exotic frame that needs > WGPU_DIAG_UBO_RING
 * distinct viewport values. GROW rather than reuse the last ring slot: because
 * wgpuQueueWriteBuffer runs before the command buffer, reusing an occupied slot
 * would retroactively rewrite an earlier draw's viewport. Created lazily, reused
 * across frames (only s_diag_ubo_used resets); the common path never touches
 * these. */
struct WgpuDiagUbo { WGPUBuffer buf; float val[4]; };
static struct WgpuDiagUbo *s_diag_ubo_ext = NULL;
static int                 s_diag_ubo_ext_cap = 0;

/* Clear color, pushed by gfx_pc.c before start_frame (see gfx_webgpu_set_clear_color). */
static double s_clear_r = 0.0, s_clear_g = 0.0, s_clear_b = 0.0;

/* Draw resources: a 1x1 white fallback texture + a nearest sampler for
 * used-but-unuploaded texture slots, and one large vertex buffer bump-allocated
 * per frame (reset in start_frame; consumed by draw_triangles). */
static WGPUTexture     s_white_tex = NULL;
static WGPUTextureView s_white_view = NULL;
static WGPUSampler     s_default_sampler = NULL;
#define WGPU_VBUF_INITIAL_CAP (16u * 1024u * 1024u)
static WGPUBuffer s_vbuf = NULL;
static uint32_t   s_vbuf_cap = 0;
static uint32_t   s_vbuf_off = 0;
static uint32_t   s_vbuf_shadow_cap = 0;
static uint64_t   s_vbuf_frame_bytes = 0;
static uint64_t   s_vbuf_bytes_high_water = 0;
static uint32_t   s_vbuf_frame_segments = 0;
static uint32_t   s_vbuf_segments_high_water = 0;
static uint32_t   s_vbuf_cap_high_water = 0;
/* WEB-023 (residual): CPU-side shadow of the per-frame vertex buffer. Every scene
 * draw (wgpu_draw_triangles) and the minimap overlay memcpy their batch into this
 * shadow at the bump offset instead of issuing a per-batch wgpuQueueWriteBuffer
 * (~100-200 of them a frame — one wasm↔JS crossing + one queue-write command each).
 * The accumulated range [0, s_vbuf_off) is uploaded ONCE in wgpu_end_frame just
 * before submit: queue writes execute before the command buffer (the WEB-053
 * ordering guarantee), so a single pre-submit upload of everything this frame's
 * draws referenced lands exactly what each draw's SetVertexBuffer(voff, bytes)
 * reads — byte-identical to the old per-batch writes, one crossing instead of many.
 * Sized to the current GPU segment and grown in lockstep with it; calloc so the
 * inter-batch alignment padding (bump-skipped, never read) is defined. If the
 * allocation fails the writers fall back to per-batch writeBuffer, so a
 * low-memory host still renders—just without the batching win. */
static uint8_t   *s_vbuf_shadow = NULL;

/* WEB-027: a small uniform (frame counter + window height) read by every
 * combiner that samples SHADER_NOISE. Bound at group(0) @binding(7) ONLY for
 * those pipelines (info->uses_noise).
 *
 * E28: this used to be ONE frame-global buffer holding s_scene_h, written once
 * from start_frame. GL does not work that way: gfx_opengl_set_uniforms() uploads
 * `current_height`, which gfx_opengl_set_viewport() sets per VIEWPORT, so a
 * split-screen frame hashes noise against each player's viewport height and the
 * full-height HUD pass against its own. Pinning WebGPU to the render-target
 * height made every viewport hash against the same number, so the dither
 * pattern differed from GL wherever the viewport was not full height.
 *
 * The fix is a per-draw-group ring, deduplicated by value: one buffer per
 * distinct {frame, height} pair this frame (in practice one per viewport), so
 * the draw bind-group cache still hits for repeated materials. Slots are GROWN
 * rather than rewritten because wgpuQueueWriteBuffer executes before the command
 * buffer — reusing an occupied slot would retroactively rewrite an earlier
 * draw's height. Same hazard, same shape, as the diag viewport ring above and
 * the E7 resolve ring. */
#define WGPU_NOISE_UBO_RING 8
struct WgpuNoiseUbo { WGPUBuffer buf; float val[4]; };
static WGPUBuffer s_noise_ubo[WGPU_NOISE_UBO_RING];
static float      s_noise_ubo_val[WGPU_NOISE_UBO_RING][4];
static int        s_noise_ubo_used = 0;   /* slots written this frame */
static struct WgpuNoiseUbo *s_noise_ubo_ext = NULL;
static int                  s_noise_ubo_ext_cap = 0;
static uint32_t   s_noise_frame = 0;
/* RL-5: one level-stable linear sun colour/strength UBO. Object-local direction
 * travels in the VBO, so this buffer never changes between draws. */
static WGPUBuffer s_light_ubo = NULL;

/*
 * Capture-and-replay sun shadows. The frontend publishes one immutable typed
 * caster frame; WebGPU consumes it before opening the ordinary scene pass.
 * The depth array and replay VBO are backend-owned, while the cascade fit is
 * shared with GL so both shipped paths use identical camera/light math.
 */
#define WGPU_SHADOW_DEPTH_FORMAT WGPUTextureFormat_Depth32Float
/* GFX_SHADOW_MAX_CASCADES 4x4 matrices, then 8 floats of receiver params. The
 * pack loop below writes matrix `n` at float offset n*16, so the two constants
 * are one layout and must move together. */
#define WGPU_SHADOW_UNIFORM_SIZE 160u
_Static_assert(
    WGPU_SHADOW_UNIFORM_SIZE ==
        (GFX_SHADOW_MAX_CASCADES * 16u + 8u) * sizeof(float),
    "shadow receiver uniform does not match GFX_SHADOW_MAX_CASCADES");
#define WGPU_SHADOW_RESOURCE_MAX_FAILURES 3
static GfxShadowPlan s_shadow_plan;
static int s_shadow_receiver_view = -1;
static WGPUTexture s_shadow_tex = NULL;
static WGPUTextureView s_shadow_array_view = NULL;
static WGPUTextureView s_shadow_layer_view[GFX_SHADOW_MAX_MAPS];
static WGPUSampler s_shadow_sampler = NULL;
static WGPUBuffer s_shadow_vbuf = NULL;
static uint64_t s_shadow_vbuf_cap = 0;
static WGPUShaderModule s_shadow_module = NULL;
static WGPUBindGroupLayout s_shadow_pass_bgl = NULL;
static WGPUPipelineLayout s_shadow_pass_layout = NULL;
static WGPURenderPipeline s_shadow_pipeline[2]; /* 0 front-cull, 1 two-sided */
static WGPUBuffer s_shadow_matrix_ubo[GFX_SHADOW_MAX_MAPS];
static WGPUBindGroup s_shadow_matrix_bg[GFX_SHADOW_MAX_MAPS];
static WGPUBuffer s_shadow_receiver_ubo[GFX_SHADOW_MAX_VIEWS];
static uint32_t s_shadow_res = 0;
static uint32_t s_shadow_layers = 0;
static uint32_t s_shadow_resource_fail_count = 0;
static uint32_t s_shadow_resource_fail_res = 0;
static uint32_t s_shadow_resource_fail_layers = 0;
static bool s_shadow_resource_perma_fail = false;
static uint64_t s_shadow_attempted_frames = 0;
static uint64_t s_shadow_complete_frames = 0;
static uint64_t s_shadow_fallback_frames = 0;
static uint64_t s_shadow_resource_failures = 0;
#ifdef __EMSCRIPTEN__
static uint32_t s_shadow_receiver_prewarm_pending = 0;
static bool s_shadow_receiver_prewarm_failed = false;
/* E11: memo for wgpu_shadow_prewarm_receivers(). The scan is
 * O(shaders x pipelines) and ran on EVERY shadow frame even once every receiver
 * twin was warm and nothing had changed. The key below is the complete set of
 * inputs that can change the scan's outcome:
 *   shaders  -- a new combiner can introduce a new receiver candidate;
 *   pipes    -- a new pipeline key on an existing shader is a new candidate,
 *               and a twin created by the scan itself lands here too, so work
 *               in progress always invalidates;
 *   pending  -- the shadow-prewarm in-flight count, which the return value
 *               reads directly;
 *   inflight -- the general async count, because a BASE pipeline flipping
 *               PENDING -> READY makes it eligible for a twin without changing
 *               either count above, and that transition is exactly where it is
 *               decremented.
 * Anything the memo cannot see (a receiver pipeline turning FAILED, the
 * permanent-failure latch) is checked before the memo is consulted. */
static bool     s_shadow_prewarm_memo_valid = false;
static bool     s_shadow_prewarm_memo_result = false;
static size_t   s_shadow_prewarm_memo_shaders = 0;
static size_t   s_shadow_prewarm_memo_pipes = 0;
static uint32_t s_shadow_prewarm_memo_pending = 0;
static int      s_shadow_prewarm_memo_inflight = 0;
#endif

static void wgpu_render_shadow_maps(void);
static void wgpu_release_shadow_resources(void);
static bool wgpu_shadow_prewarm_receivers(void);

/* WEB-052: modern-mesh (scene decor) uniform ring. One persistent uniform buffer
 * holding a ring of 256-byte-aligned slots (the WebGPU minUniformBufferOffset-
 * Alignment guarantee); each decor draw writes its 96-byte uniform into a FRESH
 * slot and binds it with a dynamic offset, so the per-mesh bind group is cached
 * (in WgpuModernEntry) and reused across frames instead of allocating a UBO + a
 * bind group per draw. The ring is reset per frame (s_modern_ubo_used = 0 in
 * start_frame) and GROWN on overflow — never reusing a slot already written this
 * frame, because wgpuQueueWriteBuffer pre-executes the command buffer (the same
 * retroactive-rewrite hazard the diag ring and WEB-053 guard). Growing recreates
 * the buffer, so cached bind groups carry a generation stamp (bg_gen) and rebuild
 * against the new buffer on first reuse. */
#define WGPU_MODERN_UBO_ALIGN 256u
#define WGPU_MODERN_UBO_INIT  64
static WGPUBuffer s_modern_ubo      = NULL;
static int        s_modern_ubo_cap  = 0;   /* slots in s_modern_ubo */
static int        s_modern_ubo_used = 0;   /* slots consumed this frame */
static uint32_t   s_modern_ubo_gen  = 0;   /* bumped on (re)create; stamps cached bgs */

/* Dynamic depth / viewport / scissor state. WebGPU bakes depth into the
 * pipeline, so the depth fields feed the pipeline cache key; viewport/scissor are
 * render-pass encoder state applied per draw. */
static bool     s_depth_test = false, s_depth_update = false, s_depth_compare = false;
static uint16_t s_zmode = 0;
static int s_vp_x = 0, s_vp_y = 0, s_vp_w = 0, s_vp_h = 0;
static int s_sc_x = 0, s_sc_y = 0, s_sc_w = 0, s_sc_h = 0;
static bool s_sc_set = false;

/* depth-clip-control granted at device creation: 3D pipelines set
 * unclippedDepth so far-plane-crossing geometry depth-clamps like GL/Metal
 * (g_depth_clamp_enabled invariance; DAM-R1). */
static bool s_unclipped_depth_supported = false;

/* PERF-005: count of async render-pipeline creations currently in flight. Bumped
 * when wgpu_pipeline_for kicks an async create (web-live only), dropped in the
 * on_pipeline_ready callback. wgpu_end_frame drains the future queue only while
 * this is > 0. Declared unconditionally: on native it stays pinned at 0 (nothing
 * ever increments it — the async kick is #ifdef __EMSCRIPTEN__), so the end-frame
 * drain check is a permanently-dead branch there and native behavior is unchanged. */
static int s_pending_pipelines = 0;
static uint64_t s_async_pipeline_creates = 0;
static uint64_t s_async_pipeline_ready = 0;
static uint64_t s_async_pipeline_failed = 0;
static uint64_t s_async_present_hold_frames = 0;
static uint32_t s_async_pending_high_water = 0;
static uint32_t s_async_pipeline_frames_max = 0;
static uint32_t s_present_hold_streak = 0;
static uint32_t s_present_hold_streak_max = 0;
#define WGPU_PRESENT_HOLD_MAX 30

#ifdef __EMSCRIPTEN__
/* Render-pipeline completions mutate the program cache.  AllowProcessEvents
 * makes their execution point explicit: only renderer-owned event drains may
 * enter the callback.  The guard is both a runtime proof and a fail-closed
 * fence against a browser binding unexpectedly dispatching it spontaneously. */
static bool s_pipeline_callback_owner_drain;
static uint64_t s_pipeline_callback_owner_event_pumps;
static uint64_t s_pipeline_callback_shutdown_late_safe;
static uint64_t s_pipeline_callback_shutdown_guarded;

static void wgpu_pipeline_callback_owner_poll(void) {
    const bool previous = s_pipeline_callback_owner_drain;
    s_pipeline_callback_owner_drain = true;
    WGPU_COMPAT_QUEUE_POLL(s_instance, s_device);
    s_pipeline_callback_owner_drain = previous;
    s_pipeline_callback_owner_event_pumps++;
}

static void wgpu_pipeline_callback_owner_drain(void) {
    const bool previous = s_pipeline_callback_owner_drain;
    s_pipeline_callback_owner_drain = true;
    WGPU_COMPAT_DRAIN(s_instance);
    s_pipeline_callback_owner_drain = previous;
    s_pipeline_callback_owner_event_pumps++;
}

static void wgpu_pipeline_callback_owner_block(void) {
    const bool previous = s_pipeline_callback_owner_drain;
    s_pipeline_callback_owner_drain = true;
    WGPU_COMPAT_QUEUE_BLOCK(s_instance, s_device);
    s_pipeline_callback_owner_drain = previous;
    s_pipeline_callback_owner_event_pumps++;
}

static void wgpu_pipeline_callback_owner_pump(WGPUInstance instance,
                                              WGPUDevice device) {
    const bool previous = s_pipeline_callback_owner_drain;
    s_pipeline_callback_owner_drain = true;
    WGPU_COMPAT_PUMP(instance, device);
    s_pipeline_callback_owner_drain = previous;
    s_pipeline_callback_owner_event_pumps++;
}
#else
static void wgpu_pipeline_callback_owner_poll(void) {
    WGPU_COMPAT_QUEUE_POLL(s_instance, s_device);
}

static void wgpu_pipeline_callback_owner_drain(void) {
    WGPU_COMPAT_DRAIN(s_instance);
}

static void wgpu_pipeline_callback_owner_block(void) {
    WGPU_COMPAT_QUEUE_BLOCK(s_instance, s_device);
}

static void wgpu_pipeline_callback_owner_pump(WGPUInstance instance,
                                              WGPUDevice device) {
    WGPU_COMPAT_PUMP(instance, device);
}
#endif

/* Every ProcessEvents call in this translation unit must pass through an owner
 * wrapper. Bring-up and readback need a yielding pump, so do not use the raw
 * compatibility wait macro there: it would dispatch pipeline completion
 * outside the program-cache ownership boundary. */
#define WGPU_PIPELINE_CALLBACK_OWNER_WAIT(condition, instance, device,      \
                                          max_iters)                         \
    do {                                                                     \
        for (unsigned _owner_wait = 0u;                                     \
             !(condition) && _owner_wait < (unsigned)(max_iters);          \
             ++_owner_wait) {                                               \
            wgpu_pipeline_callback_owner_pump((instance), (device));       \
        }                                                                    \
    } while (0)

/* PERF-005b: count of draw batches dropped THIS FRAME because their render
 * pipeline is still PENDING (async create in flight). A frame with any such
 * drop is visually incomplete — world geometry is simply missing, which on
 * screen reads as sky/backdrop "bleeding" through walls (the level-entry and
 * first-sight pop-in of PERF-005). wgpu_end_frame uses this to withhold the
 * PRESENT of incomplete frames (hold the last complete image) instead of
 * showing them; see there for the bounded-hold contract. FAILED pipelines do
 * NOT count — a permanently-failed create must present degraded rather than
 * hold forever. Native never stores a PENDING slot, so this stays 0 there. */
static int s_frame_pending_skips = 0;

/* WEB-023-lite: the viewport/scissor rect LAST APPLIED to the current render-pass
 * encoder, so wgpu_draw_triangles can skip re-emitting an unchanged rect (gfx_pc
 * re-sets the same viewport+scissor for every draw in a run — hundreds of
 * redundant Set calls per frame). These track the FINAL (Y-flipped, clamped)
 * values actually handed to the encoder. Render-pass state does NOT carry across
 * passes, so wgpu_reset_pass_dynamic_state() MUST clear the "applied" flags at
 * every pass begin (start_frame + the memory-blend split-resume). */
static bool s_vp_applied = false;
static int  s_vp_ax = 0, s_vp_ay = 0, s_vp_aw = 0, s_vp_ah = 0;
static bool s_sc_applied = false;
static int  s_sc_ax = 0, s_sc_ay = 0, s_sc_aw = 0, s_sc_ah = 0;

/* PERF-014: the render pipeline and group(0) bind group LAST APPLIED to the
 * current render-pass encoder (s_pass), so wgpu_draw_triangles can skip the
 * redundant SetPipeline/SetBindGroup that gfx_pc's per-draw-group material
 * re-setup emits (consecutive draws in a run frequently repeat the same
 * pipeline+bind group; on web each Set is a wasm↔JS crossing, ~100-200/frame).
 * NULL is the "nothing applied yet" sentinel. Like the viewport/scissor trackers
 * these are render-pass encoder state that does NOT carry across passes, so
 * wgpu_reset_pass_dynamic_state() clears them at every s_pass begin — otherwise
 * the first draw of a new pass would wrongly skip a needed SetPipeline. Note:
 * wgpu_draw_modern_mesh also writes s_pass's pipeline/bind group DIRECTLY,
 * interleaved with the triangle draws in the same scene pass and bypassing this
 * dedup, so it updates these trackers to keep the next wgpu_draw_triangles honest
 * (see there). */
static WGPURenderPipeline s_pipe_applied = NULL;
static WGPUBindGroup      s_bg_applied   = NULL;

static void wgpu_reset_pass_dynamic_state(void) {
    s_vp_applied = false;
    s_sc_applied = false;
    s_pipe_applied = NULL;   /* PERF-014: fresh pass = no pipeline bound */
    s_bg_applied   = NULL;   /* PERF-014: fresh pass = no bind group bound */
}

/* Cache eviction and redundant-bind suppression share raw WebGPU handles.
 * Never release a cache-owned handle while the current-pass tracker still
 * names it: native implementations may recycle the C handle address, turning
 * a later pointer comparison into an ABA false match. The encoder retains the
 * object it actually bound, so clearing the tracker is sufficient and forces
 * the next draw to publish its replacement explicitly. */
static void wgpu_release_cached_pipeline(WGPURenderPipeline pipeline) {
    if (pipeline == NULL) {
        return;
    }
    if (s_pipe_applied == pipeline) {
        s_pipe_applied = NULL;
    }
    wgpuRenderPipelineRelease(pipeline);
}

static void wgpu_release_cached_bind_group(WGPUBindGroup bind_group) {
    if (bind_group == NULL) {
        return;
    }
    if (s_bg_applied == bind_group) {
        s_bg_applied = NULL;
    }
    wgpuBindGroupRelease(bind_group);
}

/* ZMODE_DEC decal (gfx_opengl.c / gfx_metal.mm): coplanar decals get a negative
 * polygon offset so they win the depth test against the surface they overlay. */
static bool wgpu_depth_is_decal(void) {
    return s_zmode == 0xc00 && s_depth_test && s_depth_compare;
}

/* ------------------------------------------------------------------------
 * Async request helpers (wgpu-native fires these during processEvents), mirroring
 * the validated spike (tests/test_webgpu_spike.c).
 * ---------------------------------------------------------------------- */
static void release_adapter_request_handle(void *handle) {
    if (handle != NULL) wgpuAdapterRelease((WGPUAdapter)handle);
}

static void on_adapter(WGPURequestAdapterStatus s, WGPUAdapter a, WGPUStringView m, void *u1, void *u2) {
    (void)m;
    (void)u2;
    gfx_webgpu_async_request_complete(
        (GfxWebgpuAsyncRequest *)u1, (int)s, (void *)a,
        release_adapter_request_handle);
}

static void release_device_request_handle(void *handle) {
    if (handle != NULL) {
        wgpuDeviceDestroy((WGPUDevice)handle);
        wgpuDeviceRelease((WGPUDevice)handle);
    }
}

static void on_device(WGPURequestDeviceStatus s, WGPUDevice d, WGPUStringView m, void *u1, void *u2) {
    (void)m;
    (void)u2;
    gfx_webgpu_async_request_complete(
        (GfxWebgpuAsyncRequest *)u1, (int)s, (void *)d,
        release_device_request_handle);
}

static bool wgpu_request_adapter_attempt(
        WGPUInstance instance, const WGPURequestAdapterOptions *options,
        bool inject_failure, WGPUAdapter *out_adapter,
        WGPURequestAdapterStatus *out_status) {
    GfxWebgpuAsyncRequest *request = gfx_webgpu_async_request_create();
    if (request == NULL ||
        !gfx_webgpu_async_request_retain_callback(request)) {
        gfx_webgpu_async_request_release(request);
        return false;
    }

    WGPURequestAdapterCallbackInfo callback = {0};
    callback.mode = WGPUCallbackMode_AllowProcessEvents;
    callback.callback = on_adapter;
    callback.userdata1 = request;
    if (inject_failure) {
        on_adapter(WGPURequestAdapterStatus_Unavailable, NULL,
                   (WGPUStringView){0}, request, NULL);
    } else {
        wgpuInstanceRequestAdapter(instance, options, callback);
    }
    WGPU_PIPELINE_CALLBACK_OWNER_WAIT(
        gfx_webgpu_async_request_completed(request), instance, NULL,
        WGPU_COMPAT_BRINGUP_WAIT_ITERS);

    int status = (int)WGPURequestAdapterStatus_Unavailable;
    void *handle = NULL;
    const bool completed = gfx_webgpu_async_request_finish(
        request, &status, &handle);
    gfx_webgpu_async_request_release(request);
    if (out_status != NULL) {
        *out_status = (WGPURequestAdapterStatus)status;
    }
    if (!completed || status != (int)WGPURequestAdapterStatus_Success ||
        handle == NULL) {
        release_adapter_request_handle(handle);
        return false;
    }
    *out_adapter = (WGPUAdapter)handle;
    return true;
}

static bool wgpu_request_device_attempt(
        WGPUInstance instance, WGPUAdapter adapter,
        WGPUDeviceDescriptor *descriptor, bool inject_failure,
        WGPUDevice *out_device, uintptr_t *out_generation,
        WGPURequestDeviceStatus *out_status) {
    const uintptr_t generation = ++s_next_device_generation;
    if (!gfx_webgpu_callback_latch_begin(generation)) {
        fprintf(stderr,
                "[webgpu] no callback slot for device generation %llu\n",
                (unsigned long long)generation);
        return false;
    }

    descriptor->uncapturedErrorCallbackInfo.userdata1 = (void *)generation;
    descriptor->deviceLostCallbackInfo.userdata1 = (void *)generation;

    GfxWebgpuAsyncRequest *request = gfx_webgpu_async_request_create();
    if (request == NULL ||
        !gfx_webgpu_async_request_retain_callback(request)) {
        gfx_webgpu_async_request_release(request);
        gfx_webgpu_callback_latch_retire(generation);
        return false;
    }

    WGPURequestDeviceCallbackInfo callback = {0};
    callback.mode = WGPUCallbackMode_AllowProcessEvents;
    callback.callback = on_device;
    callback.userdata1 = request;
    if (inject_failure) {
        on_device(WGPURequestDeviceStatus_Error, NULL,
                  (WGPUStringView){0}, request, NULL);
    } else {
        wgpuAdapterRequestDevice(adapter, descriptor, callback);
    }
    WGPU_PIPELINE_CALLBACK_OWNER_WAIT(
        gfx_webgpu_async_request_completed(request), instance, NULL,
        WGPU_COMPAT_BRINGUP_WAIT_ITERS);

    int status = (int)WGPURequestDeviceStatus_Error;
    void *handle = NULL;
    const bool completed = gfx_webgpu_async_request_finish(
        request, &status, &handle);
    gfx_webgpu_async_request_release(request);
    if (out_status != NULL) {
        *out_status = (WGPURequestDeviceStatus)status;
    }
    if (!completed || status != (int)WGPURequestDeviceStatus_Success ||
        handle == NULL) {
        release_device_request_handle(handle);
        gfx_webgpu_callback_latch_retire(generation);
        return false;
    }
    *out_device = (WGPUDevice)handle;
    *out_generation = generation;
    return true;
}

static WGPUStringView wgpu_sv(const char *s) {
    WGPUStringView v; v.data = s; v.length = s ? strlen(s) : 0; return v;
}

static void wgpu_runtime_fatal(const char *message) {
    s_ready = false;
    s_runtime_status = GFX_RENDERING_FATAL;
    WGPU_COMPAT_REPORT_FAILURE(message);
}

/* Spontaneous WebGPU callbacks never mutate renderer state.  Consume their
 * generation-scoped atomic record only at a renderer-thread boundary. */
static bool wgpu_consume_callback_failure(void) {
    enum GfxWebgpuCallbackEvent event = GFX_WEBGPU_CALLBACK_NONE;
    const uintptr_t generation =
        gfx_webgpu_callback_latch_active_generation();
    if (!gfx_webgpu_callback_latch_consume(generation, &event)) {
        return false;
    }
    if (event == GFX_WEBGPU_CALLBACK_DEVICE_LOST) {
        wgpu_runtime_fatal(
            "The graphics device was lost - reload the page to continue from "
            "your last auto-save.");
    } else {
        wgpu_runtime_fatal(
            "The graphics backend reported an unrecoverable error. Reload the "
            "page to continue from the last persisted save.");
    }
    s_callback_recovery_pending = true;
    return true;
}

static uint64_t wgpu_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

static void wgpu_reset_backpressure_stats(void) {
    s_gpu_frames_in_flight = 0u;
    s_gpu_frames_in_flight_high_water = 0u;
    s_gpu_frame_submissions = 0u;
    s_gpu_frame_completions = 0u;
    s_gpu_surface_presents = 0u;
    s_gpu_surface_holds = 0u;
    s_gpu_surface_unavailable = 0u;
    s_gpu_backpressure_waits = 0u;
    s_gpu_backpressure_polls = 0u;
    s_gpu_backpressure_skips = 0u;
    s_gpu_endpoint_admission_skips = 0u;
    s_gpu_replay_admission_skips = 0u;
    s_gpu_completion_failures = 0u;
    s_gpu_abandoned_completions = 0u;
    s_gpu_backpressure_wait_ns = 0u;
    s_gpu_runtime_waits = 0u;
    s_gpu_runtime_wait_ns = 0u;
    s_gpu_first_submit_ns = 0u;
    s_gpu_last_submit_ns = 0u;
}

WGPU_COMPAT_QUEUE_DONE_CALLBACK(wgpu_on_frame_work_done) {
    const uintptr_t generation = (uintptr_t)userdata;
    WGPU_COMPAT_QUEUE_DONE_UNUSED();
    if (generation == 0u || generation != s_active_work_generation) {
        return;
    }
    if (s_gpu_frames_in_flight > 0u) {
        s_gpu_frames_in_flight--;
        s_gpu_frame_completions++;
    } else {
        s_gpu_completion_failures++;
    }
    if (status != WGPUQueueWorkDoneStatus_Success) {
        s_gpu_completion_failures++;
        fprintf(stderr,
                "[webgpu] submitted frame completion failed (status=%d)\n",
                (int)status);
        wgpu_runtime_fatal(
            "The graphics queue could not complete a submitted frame. Reload "
            "the page to continue from the last persisted save.");
    }
}

static bool wgpu_backpressure_check_below(
    unsigned limit, bool enforce, bool runtime) {
    wgpu_pipeline_callback_owner_poll();
    s_gpu_backpressure_polls++;
    if (s_gpu_frames_in_flight < limit || !enforce) {
        return true;
    }
    if (runtime || !WGPU_COMPAT_QUEUE_CAN_BLOCK) {
        s_gpu_backpressure_skips++;
        return false;
    }

    const uint64_t started = wgpu_monotonic_ns();
    unsigned stalled = 0u;
    s_gpu_backpressure_waits++;
    while (s_gpu_frames_in_flight >= limit && stalled < 8u) {
        const unsigned before = s_gpu_frames_in_flight;
        wgpu_pipeline_callback_owner_block();
        wgpu_pipeline_callback_owner_poll();
        s_gpu_backpressure_polls++;
        stalled = s_gpu_frames_in_flight < before ? 0u : stalled + 1u;
    }
    const uint64_t elapsed = wgpu_monotonic_ns() - started;
    s_gpu_backpressure_wait_ns += elapsed;
    if (runtime) {
        s_gpu_runtime_waits++;
        s_gpu_runtime_wait_ns += elapsed;
    }
    if (s_gpu_frames_in_flight >= limit) {
        s_gpu_completion_failures++;
        fprintf(stderr,
                "[webgpu] frame queue failed to retire below %u in-flight\n",
                limit);
        wgpu_runtime_fatal(
            "The graphics queue stopped completing frames. Reload the page to "
            "continue from the last persisted save.");
        return false;
    }
    return true;
}

static unsigned wgpu_backpressure_limit_before_frame(void) {
    return gfx_dkr_replay_pass_active()
        ? WGPU_FRAME_IN_FLIGHT_MAX - 1u
        : WGPU_FRAME_IN_FLIGHT_MAX;
}

static void wgpu_track_frame_submission(void) {
    const uint64_t now = wgpu_monotonic_ns();
    if (s_gpu_first_submit_ns == 0u) {
        s_gpu_first_submit_ns = now;
    }
    s_gpu_last_submit_ns = now;
    s_gpu_frame_submissions++;
    s_gpu_frames_in_flight++;
    if (s_gpu_frames_in_flight > s_gpu_frames_in_flight_high_water) {
        s_gpu_frames_in_flight_high_water = s_gpu_frames_in_flight;
    }
    WGPU_COMPAT_QUEUE_ON_DONE(
        s_queue, wgpu_on_frame_work_done,
        (void *)s_active_work_generation);

    /* Never wait after submission. The fixed-tick adapter services audio only
     * after end_frame returns, so a synchronous device drain here can starve
     * the SDL queue on a slow D3D12 completion. Admission is checked before a
     * later frame, after the completed tick refills audio. */
}

static void wgpu_abandon_in_flight(void) {
    s_gpu_abandoned_completions += s_gpu_frames_in_flight;
    s_gpu_frames_in_flight = 0u;
}

static void wgpu_report_backpressure(void) {
    uint64_t rate_millihz = 0u;
    if (s_gpu_frame_submissions > 1u &&
        s_gpu_last_submit_ns > s_gpu_first_submit_ns) {
        const uint64_t elapsed = s_gpu_last_submit_ns - s_gpu_first_submit_ns;
        const uint64_t intervals = s_gpu_frame_submissions - 1u;
        rate_millihz = elapsed > 0u &&
                intervals <= UINT64_MAX / UINT64_C(1000000000000)
            ? intervals * UINT64_C(1000000000000) / elapsed : 0u;
    }
    fprintf(stderr,
            "[WGPU-BACKPRESSURE] cap=%u submitted=%llu completed=%llu "
            "presented=%llu held=%llu unavailable=%llu "
            "inflight=%u highwater=%u waits=%llu polls=%llu skips=%llu "
            "endpointSkips=%llu replaySkips=%llu "
            "failures=%llu abandoned=%llu waitns=%llu runtimewaits=%llu "
            "runtimewaitns=%llu rateMilliHz=%llu\n",
            WGPU_FRAME_IN_FLIGHT_MAX,
            (unsigned long long)s_gpu_frame_submissions,
            (unsigned long long)s_gpu_frame_completions,
            (unsigned long long)s_gpu_surface_presents,
            (unsigned long long)s_gpu_surface_holds,
            (unsigned long long)s_gpu_surface_unavailable,
            s_gpu_frames_in_flight, s_gpu_frames_in_flight_high_water,
            (unsigned long long)s_gpu_backpressure_waits,
            (unsigned long long)s_gpu_backpressure_polls,
            (unsigned long long)s_gpu_backpressure_skips,
            (unsigned long long)s_gpu_endpoint_admission_skips,
            (unsigned long long)s_gpu_replay_admission_skips,
            (unsigned long long)s_gpu_completion_failures,
            (unsigned long long)s_gpu_abandoned_completions,
            (unsigned long long)s_gpu_backpressure_wait_ns,
            (unsigned long long)s_gpu_runtime_waits,
            (unsigned long long)s_gpu_runtime_wait_ns,
            (unsigned long long)rate_millihz);
}

static bool wgpu_submit_commands(
        WGPUQueue queue, size_t command_count,
        WGPUCommandBuffer const *commands) {
    if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_QUEUE_SUBMIT)) {
        fprintf(stderr, "[webgpu] command submission fault injected\n");
        wgpu_runtime_fatal(
            "The graphics backend could not submit work to the device. Reload "
            "the page to continue from the last persisted save.");
        return false;
    }
    wgpuQueueSubmit(queue, command_count, commands);
    return true;
}

/* Convert asynchronous WebGPU errors into a typed fatal state. Continuing after
 * a validation/OOM/internal error would turn the renderer into a silent no-op
 * while simulation advances, which is more damaging than a controlled stop. */
static void on_device_error(WGPUDevice const *device, WGPUErrorType type,
                            WGPUStringView msg, void *u1, void *u2) {
    (void)device; (void)u2;
    uintptr_t generation = (uintptr_t)u1;
    if (!gfx_webgpu_callback_latch_publish(
            generation, GFX_WEBGPU_CALLBACK_DEVICE_ERROR)) return;
    fprintf(stderr, "[webgpu] device error (type=%d): %.*s\n",
            (int)type, (int)msg.length, msg.data ? msg.data : "");
    fflush(stderr);
}

/* GPU-process restart, driver reset, or TDR. The callback only latches fatal
 * state; native recovery consumes it at a complete-frame boundary, while the
 * browser exposes the persisted-save/reload panel. */
static void on_device_lost(WGPUDevice const *device, WGPUDeviceLostReason reason,
                           WGPUStringView msg, void *u1, void *u2) {
    (void)device; (void)u2;
    uintptr_t generation = (uintptr_t)u1;
    if (reason == WGPUDeviceLostReason_Destroyed) {
        return;
    }
    if (!gfx_webgpu_callback_latch_publish(
            generation, GFX_WEBGPU_CALLBACK_DEVICE_LOST)) return;
    fprintf(stderr, "[webgpu] device lost (reason=%d): %.*s\n",
            (int)reason, (int)msg.length, msg.data ? msg.data : "");
    fflush(stderr);
}

/* ------------------------------------------------------------------------
 * Surface creation (platform-specific window -> WGPUSurface) — the dialect seam.
 *
 * One of only two inline `#ifdef __EMSCRIPTEN__` sites in this file (seam rule,
 * Task W7): the wgpu-native surface-source structs (MetalLayer / Win32 / X11 /
 * Wayland) exist only in wgpu-native's webgpu.h, so they must stay on the
 * native side of this fork; the browser builds a canvas-selector surface.
 * (The other is PERF-005's async-pipeline block at wgpu_pipeline_for /
 * on_pipeline_ready: it must be absent from the native TU so native stays
 * byte-for-byte HEAD, and the async create/callback API has no native test
 * coverage. Its per-frame completion drain lives in the compat seam as
 * WGPU_COMPAT_DRAIN.)
 *
 * Native path is parameterized by the platform handle so BOTH the engine
 * (standalone) and the app shell (AppHost, which owns the window/layer before
 * the game adopts it) create surfaces the same way. macOS uses `metal_layer`
 * (a CAMetalLayer); every other native platform uses `window` (resolved to
 * HWND/X11/Wayland by platformWebGpuWindowInfo). Browser ignores both handles
 * (the page owns exactly one canvas). Declared in gfx_webgpu_compat.h.
 *
 * Verified against the emdawnwebgpu port's webgpu.h at build time; keep
 * "#canvas" in sync with web/index.html (W5).
 * ---------------------------------------------------------------------- */
WGPUSurface wgpuCompatCreateSurface(WGPUInstance instance, void *metal_layer,
                                    struct SDL_Window *window) {
    if (instance == NULL) {
        return NULL;
    }
#ifdef __EMSCRIPTEN__
    (void)metal_layer;
    (void)window;   /* the page owns exactly one canvas */
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = {0};
    canvas.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvas.selector = (WGPUStringView){ "#canvas", WGPU_STRLEN };
    WGPUSurfaceDescriptor desc = {0};
    desc.nextInChain = &canvas.chain;
    return WGPU_FAULT_CREATE(
        BRINGUP_SURFACE, wgpuInstanceCreateSurface(instance, &desc));
#else
    WGPUSurfaceDescriptor sd = {0};
    sd.label = wgpu_sv("mdkr64-surface");
#ifdef __APPLE__
    (void)window;
    if (metal_layer == NULL) {
        fprintf(stderr, "[webgpu] no CAMetalLayer - cannot create surface\n");
        return NULL;
    }
    WGPUSurfaceSourceMetalLayer ml = {0};
    ml.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
    ml.layer = metal_layer;
    sd.nextInChain = (WGPUChainedStruct *)&ml;
    return WGPU_FAULT_CREATE(
        BRINGUP_SURFACE, wgpuInstanceCreateSurface(instance, &sd));
#else
    (void)metal_layer;
    void *display = NULL, *window_handle = NULL;
    unsigned long long win = 0;
    enum MdkrWebGpuWindowSystem sys = platformWebGpuWindowInfo(
        (void *)window, &display, &window_handle, &win);
    if (sys == MDKR_WGPU_WINDOW_WIN32) {
        WGPUSurfaceSourceWindowsHWND w = {0};
        w.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
        w.hinstance = display;
        w.hwnd = window_handle;
        sd.nextInChain = (WGPUChainedStruct *)&w;
        return WGPU_FAULT_CREATE(
            BRINGUP_SURFACE, wgpuInstanceCreateSurface(instance, &sd));
    } else if (sys == MDKR_WGPU_WINDOW_X11) {
        WGPUSurfaceSourceXlibWindow x = {0};
        x.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
        x.display = display;
        x.window = (uint64_t)win;
        sd.nextInChain = (WGPUChainedStruct *)&x;
        return WGPU_FAULT_CREATE(
            BRINGUP_SURFACE, wgpuInstanceCreateSurface(instance, &sd));
    } else if (sys == MDKR_WGPU_WINDOW_WAYLAND) {
        WGPUSurfaceSourceWaylandSurface wl = {0};
        wl.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
        wl.display = display;
        wl.surface = window_handle;
        sd.nextInChain = (WGPUChainedStruct *)&wl;
        return WGPU_FAULT_CREATE(
            BRINGUP_SURFACE, wgpuInstanceCreateSurface(instance, &sd));
    }
    fprintf(stderr,
            "[webgpu] unsupported windowing system (tag=%d) - no surface\n",
            (int)sys);
    return NULL;
#endif  /* __APPLE__ */
#endif  /* __EMSCRIPTEN__ */
}

/* Pick the swapchain format. WEB-049: the browser takes caps.formats[0] — the
 * platform's own preferred canvas format, in preference order — so an Android
 * GPU that prefers RGBA8 is not forced through a per-present BGRA8 swizzle.
 * Native keeps the long-standing BGRA8-preferring scan (the dialect flag lives
 * in gfx_webgpu_compat.h, so this file stays free of inline __EMSCRIPTEN__): the
 * offscreen scene target adopts s_surface_format and the readback swizzle keys
 * off it, so keeping the native choice pinned to BGRA8 keeps every recorded
 * baseline byte-identical (Metal already advertises BGRA8 first). Both dialects
 * fall back to BGRA8 only when the surface advertises no formats at all.
 * Parameterized so the shared bring-up helper can use it before the
 * s_surface/s_adapter statics are assigned. */
static WGPUTextureFormat wgpu_choose_format(WGPUSurface surface, WGPUAdapter adapter) {
    WGPUSurfaceCapabilities caps = {0};
    if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_CAPS_FORMAT) ||
        wgpuSurfaceGetCapabilities(surface, adapter, &caps) != WGPUStatus_Success ||
        caps.formatCount == 0) {
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        return WGPUTextureFormat_Undefined;
    }
    WGPUTextureFormat chosen = caps.formats[0];   /* platform-preferred */
    if (!WGPU_COMPAT_PREFER_FIRST_SURFACE_FORMAT) {
        for (size_t i = 0; i < caps.formatCount; ++i) {
            if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm) {
                chosen = WGPUTextureFormat_BGRA8Unorm;
                break;
            }
        }
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    return chosen;
}

/* WEB-049: choose the surface composite-alpha mode. Prefer an explicit Opaque
 * over Auto — on the browser Auto can resolve to premultiplied alpha, which
 * bleeds the page through wherever a frame's alpha carries sub-1 coverage (the
 * fence/glass RDP memory-blend surfaces). Opaque tells the compositor to ignore
 * frame alpha. Fall back to Auto only when the surface does not advertise Opaque
 * (Auto is guaranteed valid). Resolve against the active adapter/surface on
 * every configuration: native recovery may replace them with a generation
 * advertising a narrower capability set. */
static WGPUCompositeAlphaMode wgpu_choose_alpha_mode(void) {
    WGPUCompositeAlphaMode mode = WGPUCompositeAlphaMode_Auto;
    if (s_surface == NULL || s_adapter == NULL) {
        return mode;   /* Auto — caps unavailable */
    }
    WGPUSurfaceCapabilities caps = {0};
    if (!gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_CAPS_ALPHA) &&
        wgpuSurfaceGetCapabilities(s_surface, s_adapter, &caps) ==
            WGPUStatus_Success) {
        bool opaque_advertised = false;
        for (size_t i = 0; i < caps.alphaModeCount; ++i) {
            if (caps.alphaModes[i] == WGPUCompositeAlphaMode_Opaque) {
                opaque_advertised = true;
                break;
            }
        }
        mode = (WGPUCompositeAlphaMode)gfx_webgpu_surface_select_alpha(
            (uint32_t)WGPUCompositeAlphaMode_Auto,
            (uint32_t)WGPUCompositeAlphaMode_Opaque,
            opaque_advertised);
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    return mode;
}

/* PERF-051: present-mode selection. FIFO (vsync) is the default and the
 * byte-identity baseline — it matches the GL/Metal swap and is the only mode the
 * WebGPU spec guarantees the surface advertises.
 *
 * The latched presentation policy chooses between FIFO and mailbox
 * (gfx_webgpu_surface_rank_present): a cap at or below the display is paced
 * exactly by the software deadline grid on top of the FIFO queue, while a rate
 * the display cannot show — a cap above it, or uncapped — needs a queue that
 * replaces an undisplayed image instead of stalling. Immediate scans out a
 * partial frame and is therefore never a consequence of a rate: it is reachable
 * only through Video.AllowTearing, and a surface that does not advertise it
 * keeps the policy's own tear-free mode.
 *
 * GE007_WEBGPU_PRESENT remains the diagnostic override and still wins over the
 * policy:
 *     fifo | mailbox | immediate
 * It names a mode instead of a policy, but goes through the same capability
 * query and the same ranking, so a mode the surface does not advertise falls
 * back to FIFO with one stderr note rather than failing wgpuSurfaceConfigure.
 * The requested mode is process-constant; support is resolved per active
 * surface generation.
 *
 * Web: the browser present is rAF-driven (emdawnwebgpu no-ops the present, see
 * gfx_webgpu_compat.h), so present mode is a native concern; the knob is harmless
 * there by construction — an unset env keeps FIFO (byte-identical to today), and the
 * env is never set on the web build. */
static bool wgpu_present_support(GfxWebgpuPresentSupport *out) {
    WGPUSurfaceCapabilities caps = {0};
    size_t i;

    out->mailbox = false;
    out->immediate = false;
    /* Nothing was queried on these paths, so there are no members to free —
     * handing the zero-initialised struct to the freer is not its contract. */
    if (s_surface == NULL || s_adapter == NULL ||
        gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_CAPS_PRESENT)) {
        return false;
    }
    if (wgpuSurfaceGetCapabilities(s_surface, s_adapter, &caps) !=
            WGPUStatus_Success) {
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        return false;
    }
    for (i = 0; i < caps.presentModeCount; ++i) {
        if (caps.presentModes[i] == WGPUPresentMode_Mailbox) {
            out->mailbox = true;
        } else if (caps.presentModes[i] == WGPUPresentMode_Immediate) {
            out->immediate = true;
        }
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    return true;
}

static WGPUPresentMode wgpu_present_mode_for(GfxWebgpuPresentMode mode) {
    switch (mode) {
        case GFX_WEBGPU_PRESENT_MAILBOX:   return WGPUPresentMode_Mailbox;
        case GFX_WEBGPU_PRESENT_IMMEDIATE: return WGPUPresentMode_Immediate;
        case GFX_WEBGPU_PRESENT_FIFO:
        default:                           return WGPUPresentMode_Fifo;
    }
}

static WGPUPresentMode wgpu_choose_present_mode(void) {
    WGPUPresentMode mode = WGPUPresentMode_Fifo;
    const char *want = getenv("GE007_WEBGPU_PRESENT");
    const bool diagnostic_override = want != NULL && want[0] != '\0';
    GfxWebgpuPresentMode overrideRequested;
    GfxWebgpuPresentMode overrideEffective;
    GfxWebgpuPresentSupport advertised;
    bool caps_known;
#ifdef __EMSCRIPTEN__
    if (!diagnostic_override) {
        /* Browser canvas presentation is owned by rAF/the user agent. Numeric
         * caps skip rAF opportunities in the host clock; they do not request a
         * native immediate swapchain the browser cannot expose, so nothing
         * here can tear. */
        fprintf(stderr,
                "[PRESENT-MODE] backend=webgpu platform=web requestedPolicy=%s "
                "effectivePolicy=%s rate=%u tearing=0 requested=fifo "
                "effective=fifo supported=1 reason=raf-ceiling\n",
                present_sched_present_requested_policy_name(),
                present_sched_present_policy_name(),
                present_sched_present_rate());
        return mode;
    }
#endif
    if (!diagnostic_override) {
        const MdkrPresentSync sync =
            present_sched_present_sync(platform_present_display_rate());
        const bool tearing = present_sched_allow_tearing();
        const GfxWebgpuPresentMode requested =
            gfx_webgpu_surface_request_present(sync, tearing);
        GfxWebgpuPresentMode effective;

        caps_known = wgpu_present_support(&advertised);
        effective = gfx_webgpu_surface_rank_present(sync, tearing, advertised);
        fprintf(stderr,
                "[PRESENT-MODE] backend=webgpu policy=%s rate=%u "
                "displayHz=%u tearing=%d requested=%s effective=%s "
                "supported=%d override=0%s\n",
                present_sched_present_policy_name(),
                present_sched_present_rate(),
                platform_present_display_rate(), tearing ? 1 : 0,
                gfx_webgpu_surface_present_name(requested),
                gfx_webgpu_surface_present_name(effective),
                requested == effective ? 1 : 0,
                requested == effective ? ""
                    : (caps_known ? " reason=capability-fallback"
                                  : " reason=capabilities-unavailable"));
        return wgpu_present_mode_for(effective);
    }
    if (strcmp(want, "fifo") == 0) {
        overrideRequested = GFX_WEBGPU_PRESENT_FIFO;
    } else if (strcmp(want, "mailbox") == 0) {
        overrideRequested = GFX_WEBGPU_PRESENT_MAILBOX;
    } else if (strcmp(want, "immediate") == 0) {
        overrideRequested = GFX_WEBGPU_PRESENT_IMMEDIATE;
    } else {
        fprintf(stderr, "[webgpu] GE007_WEBGPU_PRESENT='%s' unrecognized "
                        "(fifo|mailbox|immediate); using fifo\n", want);
        return mode;
    }
    /* Same capability query and same ranking as the policy path — a
     * non-advertised presentMode is a wgpuSurfaceConfigure validation error,
     * so caps unavailable keeps FIFO. */
    caps_known = wgpu_present_support(&advertised);
    overrideEffective =
        gfx_webgpu_surface_rank_override(overrideRequested, advertised);
    fprintf(stderr,
            "[PRESENT-MODE] backend=webgpu policy=%s rate=%u "
            "displayHz=%u tearing=%d requested=%s effective=%s "
            "supported=%d override=1%s\n",
            present_sched_present_policy_name(),
            present_sched_present_rate(),
            platform_present_display_rate(),
            overrideEffective == GFX_WEBGPU_PRESENT_IMMEDIATE ? 1 : 0,
            gfx_webgpu_surface_present_name(overrideRequested),
            gfx_webgpu_surface_present_name(overrideEffective),
            overrideRequested == overrideEffective ? 1 : 0,
            overrideRequested == overrideEffective ? ""
                : (caps_known ? " reason=capability-fallback"
                              : " reason=capabilities-unavailable"));
    return wgpu_present_mode_for(overrideEffective);
}

/* Whether an 8-bit color target stores B,G,R,A (vs R,G,B,A) — so the readback
 * paths extract RGB correctly. BGRA8 is what wgpu_choose_format prefers and what
 * every current target advertises, but a platform that only offers RGBA8Unorm
 * would otherwise get R/B-swapped screenshots. */
static bool wgpu_format_is_bgra(WGPUTextureFormat f) {
    return f == WGPUTextureFormat_BGRA8Unorm || f == WGPUTextureFormat_BGRA8UnormSrgb;
}

static int wgpu_dump_surface_frame(void);   /* GE007_WEBGPU_DUMP_SURFACE target, or -1 */

void gfx_webgpu_request_surface_reconfigure(void) {
    s_cfg_present_mode_dirty = true;
}

static bool wgpu_configure_surface(uint32_t w, uint32_t h) {
    if (s_surface == NULL || s_device == NULL || w == 0 || h == 0) {
        return false;
    }
    WGPUSurfaceConfiguration cfg = {0};
    WGPUSurfaceCapabilities caps = {0};
    bool format_supported = false;
    size_t i;
    /* Consume the request here rather than on success: this call re-ranks the
     * present mode either way, and a configuration that fails is fatal at the
     * caller rather than something to retry. */
    s_cfg_present_mode_dirty = false;
    cfg.device = s_device;
    cfg.format = s_surface_format;
    /* The scene is rendered offscreen and copied here at present, so the surface
     * only needs to be a copy destination (plus RenderAttachment, which surfaces
     * require). If a platform disallows CopyDst, wgpuSurfaceConfigure raises a
     * device error that on_device_error logs (never aborts) and present is
     * skipped — offscreen rendering + readback still work. */
    /* CopyDst receives the scene blit on the offscreen present path (always kept, as
     * any frame may take it). CopySrc lets GE007_WEBGPU_DUMP_SURFACE read the presented
     * frame (scene + overlay) back — requested ONLY when that dump is armed (PERF-008):
     * a permanently CopySrc surface can block a compositor fast-path on some platforms. */
    cfg.usage = WGPUTextureUsage_RenderAttachment;
    if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_CAPS_CONFIGURE) ||
        wgpuSurfaceGetCapabilities(s_surface, s_adapter, &caps) !=
        WGPUStatus_Success) {
        fprintf(stderr, "[webgpu] could not query surface capabilities\n");
        return false;
    }
    for (i = 0; i < caps.formatCount; i++) {
        if (caps.formats[i] == cfg.format) {
            format_supported = true;
            break;
        }
    }
    /*
     * Cross-dialect positive control: the browser compatibility layer can
     * already narrow its reported usages to RenderAttachment with this value.
     * Apply the same restriction here so the native recovery gate exercises
     * the real render-pass blit instead of depending on a particular adapter
     * omitting CopyDst. This may only remove optional usages; it cannot invent
     * a capability or change production behavior when the test variable is
     * absent.
     */
    const char *test_usages = getenv("MDKR_TEST_WEBGPU_SURFACE_USAGES");
    bool test_attachment_only =
        test_usages != NULL && strcmp(test_usages, "attachment") == 0;
    if (!test_attachment_only &&
        WGPU_COMPAT_SURFACE_USAGES_REPORTED && format_supported &&
        (caps.usages & WGPUTextureUsage_CopyDst) != 0) {
        cfg.usage |= WGPUTextureUsage_CopyDst;
        s_surface_copy_dst = true;
    } else {
        s_surface_copy_dst = false;
    }
    if (wgpu_dump_surface_frame() >= 0) {
        if (!test_attachment_only &&
            WGPU_COMPAT_SURFACE_USAGES_REPORTED &&
            (caps.usages & WGPUTextureUsage_CopySrc) != 0) {
            cfg.usage |= WGPUTextureUsage_CopySrc;
            s_surface_copy_src = true;
        } else {
            s_surface_copy_src = false;
            fprintf(stderr,
                    "[webgpu] surface CopySrc is unavailable; the requested "
                    "surface dump will be skipped\n");
        }
    } else {
        s_surface_copy_src = false;
    }
    if (!format_supported ||
        (WGPU_COMPAT_SURFACE_USAGES_REPORTED &&
         (caps.usages & WGPUTextureUsage_RenderAttachment) == 0)) {
        fprintf(stderr,
                "[webgpu] surface does not support format=%d usage=0x%llx "
                "(available=0x%llx)\n",
                (int)cfg.format, (unsigned long long)cfg.usage,
                (unsigned long long)caps.usages);
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        return false;
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    cfg.width = w;
    cfg.height = h;
    cfg.alphaMode = wgpu_choose_alpha_mode();   /* WEB-049: Opaque when advertised, else Auto */
    cfg.presentMode = wgpu_choose_present_mode();   /* PERF-051: FIFO default (vsync), GE007_WEBGPU_PRESENT opt-in */
    if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_CONFIGURE)) {
        return false;
    }
    wgpuSurfaceConfigure(s_surface, &cfg);
    if (s_runtime_status == GFX_RENDERING_FATAL) {
        return false;
    }
    s_cfg_w = w;
    s_cfg_h = h;
    return true;
}

/* Full WebGPU bring-up for a native window handle: instance -> surface ->
 * adapter -> device -> queue -> surface format, mirroring the validated spike
 * (tests/test_webgpu_spike.c). On success returns true with every out-param set
 * (the caller owns the returned objects); on any failure returns false, the
 * out-params untouched, and the caller treats the backend as inert. Shared by
 * the engine's own wgpu_init AND the app shell (AppHost, via gfx_webgpu.h) so
 * the request sequence + error callback live in exactly one place. */
bool gfx_webgpu_bringup(void *metal_layer, void *sdl_window,
                        WGPUInstance *out_instance, WGPUAdapter *out_adapter,
                        WGPUDevice *out_device, WGPUQueue *out_queue,
                        WGPUSurface *out_surface, int *out_format) {
    /* WEB-026: acquire in order (instance -> surface -> adapter), tracking each
     * handle so any failure path releases what it acquired (goto fail) instead
     * of leaking. All three start NULL so a NULL-guarded cleanup is safe from
     * every early exit. */
    WGPUInstance instance = NULL;
    WGPUSurface  surface  = NULL;
    WGPUAdapter  adapter  = NULL;
    WGPUDevice   device   = NULL;
    WGPUQueue    queue    = NULL;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    uintptr_t device_generation = 0;

    instance = WGPU_FAULT_CREATE(
        BRINGUP_INSTANCE, wgpuCreateInstance(NULL));
    if (instance == NULL) {
        fprintf(stderr, "[webgpu] wgpuCreateInstance failed\n");
        return false;   /* nothing acquired yet */
    }

    /* Surface first, so it can be passed as the adapter's compatibleSurface.
     * Routed through the dialect seam (native window vs browser canvas). */
    surface = wgpuCompatCreateSurface(instance, metal_layer, sdl_window);
    if (surface == NULL) {
        fprintf(stderr, "[webgpu] surface creation failed - backend inert\n");
        goto fail;
    }

    /* Each request attempt has a distinct refcounted callback context. A timed
     * out callback can therefore resolve after this bring-up, its retry, or a
     * later recovery without writing into reused state; a late success releases
     * the adapter in the callback. */
    WGPURequestAdapterOptions aopts = {0};
    aopts.compatibleSurface = surface;
    aopts.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterStatus adapter_status =
        WGPURequestAdapterStatus_Unavailable;
    if (!wgpu_request_adapter_attempt(
            instance, &aopts,
            gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_BRINGUP_ADAPTER),
            &adapter, &adapter_status)) {
        fprintf(stderr, "[webgpu] adapter request failed (status=%d)\n",
                (int)adapter_status);
        goto fail;
    }

    WGPUAdapterInfo info = {0};
    wgpuAdapterGetInfo(adapter, &info);
    fprintf(stderr, "[webgpu] adapter backend=%d device=%.*s\n",
            (int)info.backendType, (int)info.device.length,
            info.device.data ? info.device.data : "");
    wgpuAdapterInfoFreeMembers(info);

    /* WEB-015: raise maxTextureDimension2D from WebGPU's 8192 default to the
     * adapter's real maximum. Query the adapter, ask the device for up to that
     * in requiredLimits (all other fields stay UNDEFINED = library defaults), so
     * large viewports at the default RenderScale 2 stop hitting the 8192 clamp.
     * WGPU_LIMITS_INIT leaves every field UNDEFINED; a {0}-init would instead
     * REQUIRE 0 for every limit and fail device creation. */
    WGPULimits adapter_limits = WGPU_LIMITS_INIT;
    uint32_t want_max_tex = 8192;
    if (wgpuAdapterGetLimits(adapter, &adapter_limits) == WGPUStatus_Success &&
        adapter_limits.maxTextureDimension2D > want_max_tex) {
        want_max_tex = adapter_limits.maxTextureDimension2D;
    }
    WGPULimits required_limits = WGPU_LIMITS_INIT;
    required_limits.maxTextureDimension2D = want_max_tex;

    WGPUDeviceDescriptor ddesc = {0};
    ddesc.label = wgpu_sv("mdkr64-device");
    ddesc.requiredLimits = &required_limits;
    /* DAM-R1 root cause (DAM_PARITY_DEEP_DIVE 2026-07-17 §4.1): gfx_init sets
     * g_depth_clamp_enabled=true for this backend (sim-hash invariance — the CPU
     * clipper then passes far-plane-crossing triangles through, exactly like the
     * GL/Metal depth-clamp paths), but WebGPU's DEFAULT primitive state clips
     * depth — so distant horizon geometry silently vanished (sky slivers over
     * the Dam cliffs; any far terrain on any level). Make the claim honest:
     * request depth-clip-control and set unclippedDepth on the 3D pipelines. */
    WGPUFeatureName required_features[1];
    s_unclipped_depth_supported =
        !gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_CAPS_DEPTH_CLIP_ABSENT) &&
        wgpuAdapterHasFeature(adapter, WGPUFeatureName_DepthClipControl) != 0;
    if (s_unclipped_depth_supported) {
        required_features[0] = WGPUFeatureName_DepthClipControl;
        ddesc.requiredFeatures = required_features;
        ddesc.requiredFeatureCount = 1;
    } else {
        fprintf(stderr,
                "[webgpu] adapter lacks depth-clip-control: far-plane-crossing "
                "geometry will use the frontend's homogeneous-z clamp\n");
    }
    ddesc.uncapturedErrorCallbackInfo.callback = on_device_error;
    /* WEB-025: register the device-lost callback at creation so a later GPU loss
     * (process restart / driver reset) surfaces a reload panel instead of a
     * frozen canvas. AllowSpontaneous: it may fire at any time, not only inside
     * a ProcessEvents pump. */
    ddesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    ddesc.deviceLostCallbackInfo.callback = on_device_lost;
    /*
     * Selecting the fallback-specific point also forces entry into the fallback
     * without consuming its occurrence. Otherwise a capable adapter would
     * never execute that conditional path, making the point untestable alone.
     */
    bool test_default_device_path = gfx_webgpu_fault_selected(
        GFX_WEBGPU_FAULT_BRINGUP_DEVICE_DEFAULTS);
    WGPURequestDeviceStatus device_status = WGPURequestDeviceStatus_Error;
    if (!wgpu_request_device_attempt(
            instance, adapter, &ddesc,
            test_default_device_path ||
                gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_BRINGUP_DEVICE_LIMITS),
            &device, &device_generation, &device_status)) {
        /* WEB-015/WEB-026: a blocklisted or limited adapter can reject the raised
         * maxTextureDimension2D. Retry once with library-default limits so device
         * creation still succeeds (at the 8192 default cap; s_max_tex_dim stays
         * at its floor). Covers native wgpu-native and web alike. */
        fprintf(stderr, "[webgpu] device request failed (status=%d); retrying with default limits\n",
                (int)device_status);
        ddesc.requiredLimits = NULL;
        if (!wgpu_request_device_attempt(
                instance, adapter, &ddesc,
                gfx_webgpu_fault_hit(
                    GFX_WEBGPU_FAULT_BRINGUP_DEVICE_DEFAULTS),
                &device, &device_generation, &device_status)) {
            fprintf(stderr, "[webgpu] device request failed after default-limits retry (status=%d)\n",
                    (int)device_status);
            goto fail;
        }
    }

    /* WEB-015: record the max texture dimension the device actually granted, so
     * gfx_webgpu_max_offscreen_dim() and the upload reject use the true cap. */
    {
        WGPULimits granted = WGPU_LIMITS_INIT;
        if (wgpuDeviceGetLimits(device, &granted) == WGPUStatus_Success &&
            granted.maxTextureDimension2D >= 8192) {
            s_max_tex_dim = granted.maxTextureDimension2D;
        }
        fprintf(stderr, "[webgpu] maxTextureDimension2D=%u (default 8192)\n",
                (unsigned)s_max_tex_dim);
    }

    queue = WGPU_FAULT_CREATE(BRINGUP_QUEUE, wgpuDeviceGetQueue(device));
    format = wgpu_choose_format(surface, adapter);
    if (queue == NULL || format == WGPUTextureFormat_Undefined) {
        fprintf(stderr,
                "[webgpu] device has no queue or surface has no usable format\n");
        goto fail;
    }

    /* Commit only after queue and surface format validation. Any loss/error
     * delivered from request completion through this point was retained in the
     * provisional slot; the prior live device remained active throughout. */
    if (!gfx_webgpu_callback_latch_commit(device_generation)) {
        fprintf(stderr,
                "[webgpu] device callback generation could not be committed\n");
        goto fail;
    }

#ifndef __EMSCRIPTEN__
    {
        const char *candidate_failure =
            getenv("MDKR_TEST_WEBGPU_RECOVERY_FAIL");
        if (s_native_recovery_attempted && candidate_failure != NULL &&
            strcmp(candidate_failure, "candidate-callback") == 0) {
            (void)gfx_webgpu_callback_latch_publish(
                device_generation, GFX_WEBGPU_CALLBACK_DEVICE_LOST);
        }
    }
#endif
    {
        enum GfxWebgpuCallbackEvent candidate_event =
            GFX_WEBGPU_CALLBACK_NONE;
        if (gfx_webgpu_callback_latch_consume(
                device_generation, &candidate_event)) {
            fprintf(stderr,
                    "[webgpu] candidate device failed before root transaction "
                    "commit (event=%d)\n",
                    (int)candidate_event);
            goto fail;
        }
    }

    /* Remember the exact device whose immutable callbacks carry
     * device_generation. A borrowed engine session must preserve this token; it
     * is cleared only when the owner releases the actual device. */
    s_callback_device = device;

    *out_instance = instance;
    *out_adapter  = adapter;
    *out_device   = device;
    *out_queue    = queue;
    *out_surface  = surface;
    *out_format   = (int)format;
    return true;

    /* WEB-026: release everything acquired so far on any failure (the P3 leak
     * fold-in). NULL-guarded, so it is safe from every early exit above. */
fail:
    gfx_webgpu_callback_latch_retire(device_generation);
    if (queue    != NULL) wgpuQueueRelease(queue);
    if (device   != NULL) wgpuDeviceRelease(device);
    if (adapter  != NULL) wgpuAdapterRelease(adapter);
    if (surface  != NULL) wgpuSurfaceRelease(surface);
    if (instance != NULL) wgpuInstanceRelease(instance);
    return false;
}

bool gfx_webgpu_device_failed(void) {
    (void)wgpu_consume_callback_failure();
    return s_runtime_status == GFX_RENDERING_FATAL;
}

void gfx_webgpu_host_device_will_release(WGPUDevice device) {
    if (device != NULL && device == s_callback_device) {
        /* Invalidate the immutable device callbacks before the owner destroys
         * the root. Destroyed callbacks and any delayed validation report then
         * belong to no live device generation. */
        gfx_webgpu_callback_latch_retire(
            gfx_webgpu_callback_latch_active_generation());
        s_callback_device = NULL;
    }
}

/* ------------------------------------------------------------------------
 * Vtable: init / resize / frame lifecycle
 * ---------------------------------------------------------------------- */
static bool wgpu_init(void) {
    s_ready = false;
    s_runtime_status = GFX_RENDERING_UNINITIALIZED;
    wgpu_reset_backpressure_stats();
    s_surface_recovery_attempts = 0;
    s_native_recovery_attempted = false;
    s_callback_recovery_pending = false;
    s_active_work_generation = ++s_next_device_generation;
    if (!gfx_webgpu_fault_configure()) {
        fprintf(stderr, "[webgpu] invalid fault configuration: %s\n",
                gfx_webgpu_fault_error());
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics fault-test configuration is invalid.");
        return false;
    }

    /* App-shell handoff: the launcher already stood up the WebGPU device and
     * surface for its own UI; adopt them wholesale so the game and launcher
     * share one device/surface (no second present target, no ownership war).
     * We do NOT own these objects — teardown must leave them to the shell. */
    if (platformHasHostWebGpu()) {
        s_instance       = (WGPUInstance)platformHostWgpuInstance();
        s_adapter        = (WGPUAdapter)platformHostWgpuAdapter();
        s_device         = (WGPUDevice)platformHostWgpuDevice();
        s_queue          = (WGPUQueue)platformHostWgpuQueue();
        s_surface        = (WGPUSurface)platformHostWgpuSurface();
        s_surface_format = (WGPUTextureFormat)platformHostWgpuSurfaceFormat();
        s_owns_device    = false;
        if (s_instance == NULL || s_adapter == NULL || s_device == NULL ||
            s_queue == NULL || s_surface == NULL) {
            fprintf(stderr, "[webgpu] host handoff incomplete - backend inert\n");
            WGPU_COMPAT_REPORT_FAILURE(
                "The graphics device could not be started (incomplete handoff). "
                "Reload the page to try again.");
            return false;
        }
        if (s_surface_format == WGPUTextureFormat_Undefined) {
            fprintf(stderr, "[webgpu] host handoff has no surface format\n");
            return false;
        }
        /* bringup() installed immutable device callbacks before the handoff.
         * Keep that device-lifetime generation intact. The separate work token
         * above protects queue/pipeline callbacks carrying session storage. */
        if (s_callback_device != s_device ||
            gfx_webgpu_callback_latch_active_generation() == 0) {
            fprintf(stderr,
                    "[webgpu] host handoff lost its device callback lifetime\n");
            WGPU_COMPAT_REPORT_FAILURE(
                "The graphics device handoff was incomplete. Restart the app.");
            return false;
        }
        s_ready = true;
        s_runtime_status = GFX_RENDERING_READY;
        fprintf(stderr, "[webgpu] adopted host device/surface (format=%d)\n",
                (int)s_surface_format);
        return true;
    }

    s_owns_device = true;
    void *layer = NULL;
#ifdef __APPLE__
    layer = platformGetMetalLayer();
#endif
    int fmt = 0;
    if (!gfx_webgpu_bringup(layer, platformGetSdlWindow(),
                            &s_instance, &s_adapter, &s_device, &s_queue,
                            &s_surface, &fmt)) {
        /* helper logged the specific failure; backend stays inert. Surface a
         * human-readable message to the JS shell (WEB-003) so the user isn't
         * left staring at a permanently black canvas. */
        WGPU_COMPAT_REPORT_FAILURE(
            "Your browser exposes WebGPU but no usable GPU device could be "
            "created (it may be blocklisted, disabled, or unsupported). "
            "The game can't render here.");
        return false;
    }
    s_surface_format = (WGPUTextureFormat)fmt;
    s_ready = true;
    s_runtime_status = GFX_RENDERING_READY;
    fprintf(stderr, "[webgpu] backend initialized (surface format=%d)\n", (int)s_surface_format);
    return true;
}

static void wgpu_on_resize(void) {
    /*
     * start_frame observes gfx_output_dimensions directly and owns the
     * three-stable-frame debounce plus transactional target replacement.
     * Clearing s_cfg_w/h here used to bypass that debounce on every frontend
     * resize notification, forcing the expensive allocation immediately.
     */
}

/* WEB-027: advance the per-frame noise seed and hold the ring's first slot. The
 * uniform CONTENTS are claimed per draw group (wgpu_noise_ubo), because their
 * second component is the viewport height, which GL re-uploads on every
 * viewport change — but slot 0 is still created eagerly here, exactly as the E7
 * resolve ring keeps s_resolve_ubuf as its slot 0. That keeps the buffer's
 * creation on the unconditional frame route: a scene that happens to draw no
 * noise combiner must still exercise (and fail closed on) this allocation, which
 * is what the frame.noise-buffer fault point is for. */
static void wgpu_update_noise_ubo(void) {
    if (!s_ready) {
        return;
    }
    if (s_noise_ubo[0] == NULL) {
        WGPUBufferDescriptor bd = {0};
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bd.size = 16;
        s_noise_ubo[0] = WGPU_FAULT_CREATE(
            NOISE_BUFFER, wgpuDeviceCreateBuffer(s_device, &bd));
        if (s_noise_ubo[0] == NULL) {
            fprintf(stderr, "[webgpu] noise-uniform allocation failed\n");
            wgpu_runtime_fatal(
                "The graphics backend could not allocate its frame uniform. "
                "Reload the page to continue from the last persisted save.");
            return;
        }
    }
    /* A presentation replay redraws the SAME tick's display list; advancing
     * the noise seed for it would move every dithered texel and break the
     * zero-delta replay's pixel identity. Mirrors the GL backend's frame_count
     * guard. */
    if (!gfx_dkr_replay_pass_active()) {
        s_noise_frame++;
    }
}

static void wgpu_update_light_ubo(void) {
    if (!s_ready || !g_pcLevelLightingValid) {
        return;
    }
    if (s_light_ubo == NULL) {
        WGPUBufferDescriptor bd = {0};
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bd.size = 16;
        s_light_ubo = wgpuDeviceCreateBuffer(s_device, &bd);
        if (s_light_ubo == NULL) {
            return;
        }
    }
    float params[4] = {
        g_pc_sun_color_linear[0],
        g_pc_sun_color_linear[1],
        g_pc_sun_color_linear[2],
        g_pcSunStrength,
    };
    wgpuQueueWriteBuffer(s_queue, s_light_ubo, 0,
                         params, sizeof(params));
}

static bool wgpu_start_frame(void) {
    const bool replay = gfx_dkr_replay_pass_active();
    (void)wgpu_consume_callback_failure();
    s_frame_open = false;
    s_output_overlay_active = false;
    g_pc_shadow_map_ready = 0;
    g_pc_shadow_mat_valid = 0;
    g_pc_shadow_view_ready_mask = 0;
    memset(&s_shadow_plan, 0, sizeof(s_shadow_plan));
    s_shadow_receiver_view = -1;
    s_vbuf_off = 0;   /* reset the per-frame vertex bump allocator */
    s_vbuf_frame_bytes = 0;
    s_vbuf_frame_segments = 0;
    s_sc_set = false; /* scissor is re-established by gfx_pc each frame */
    s_diag_ubo_used = 0; /* reset the per-frame viewport-UBO ring */
    s_noise_ubo_used = 0; /* E28: reset the per-frame noise-UBO ring */
    s_resolve_ubo_used = 0; /* reset the per-frame resolve-UBO ring */
    s_modern_ubo_used = 0; /* WEB-052: reset the per-frame modern-mesh UBO ring */
    wgpu_reset_pass_dynamic_state(); /* WEB-023-lite: fresh pass = no rect applied yet */
    if (!s_ready) {
        return false;
    }
    if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_DEVICE_LOST)) {
        on_device_lost(
            &s_device, WGPUDeviceLostReason_Unknown,
            wgpu_sv("deterministic MDKR_WEBGPU_FAULT injection"),
            (void *)gfx_webgpu_callback_latch_active_generation(), NULL);
        (void)wgpu_consume_callback_failure();
        return false;
    }
    /* Ordinary gameplay must never wait for the GPU. An explicitly requested
     * diagnostic dump is different: it promises the image named by this exact
     * frame. Allow the existing bounded non-runtime drain during its short
     * evidence-only pre-roll so A/B arms cannot inherit differently aged held
     * endpoints, while leaving every ordinary runtime opportunity nonblocking. */
    const bool dump_due = platform_frame_dump_due() != 0;
    const bool dump_prepare_due = platform_frame_dump_prepare_due() != 0;
    /* Coverage/equality instruments that count every frontend walk can opt
     * into deterministic full admission. This is deliberately a TEST seam:
     * production runtime and its backpressure gates never set it and therefore
     * retain the strict nonblocking contract. */
    const bool test_full_admission =
        getenv("MDKR_TEST_RENDER_FULL_ADMISSION") != NULL;
    const bool runtime_admission =
        !(dump_prepare_due || test_full_admission);
    if (dump_due && getenv("MDKR_FRAME_DUMP_TRACE") != NULL) {
        fprintf(stderr,
                "[FRAME-DUMP] WebGPU admission frame=%d inFlight=%u "
                "submitted=%llu completed=%llu\n",
                g_frameCounter, s_gpu_frames_in_flight,
                (unsigned long long)s_gpu_frame_submissions,
                (unsigned long long)s_gpu_frame_completions);
    }
    if (!wgpu_backpressure_check_below(
            wgpu_backpressure_limit_before_frame(), true,
            runtime_admission)) {
        /* Return without opening an encoder so the scheduler stays responsive.
         * Replays reserve one slot for the next authored endpoint; authored
         * frames may use both slots but never block for either one. */
        if (replay) {
            s_gpu_replay_admission_skips++;
        } else {
            s_gpu_endpoint_admission_skips++;
        }
        return false;
    }

    /* Render at the frontend's resolution (gfx_current_dimensions) — the same
     * resolution the viewports and T&L are computed against, exactly like the
     * Metal backend. */
    /*
     * DEBOUNCE ON THE OUTPUT SIZE, not the render size.
     *
     * s_cfg_w/h track the configured SURFACE, which follows the output size.
     * Debouncing gfx_current_dimensions (the RENDER size) against them meant
     * `req == s_cfg` could never hold once Video.RenderScale > 1, so every
     * frame fell into the debounce branch and the scene target oscillated
     * between the output size and the render size every few frames --
     * recreating the offscreen targets constantly and presenting frames whose
     * scene had been rendered at the wrong resolution.
     *
     * The requested size is therefore the output size; the scene is derived
     * from the COMMITTED output size by the same scale, so the two can never
     * disagree.
     */
    uint32_t req_w = gfx_output_dimensions.width;
    uint32_t req_h = gfx_output_dimensions.height;
    if (req_w == 0 || req_h == 0) {
        req_w = gfx_current_dimensions.width;
        req_h = gfx_current_dimensions.height;
    }
    if (req_w == 0 || req_h == 0) {
        return false; /* dimensions not established yet — skip cleanly */
    }

    /* PERF-020: debounce window-drag resizes. A native window drag changes
     * gfx_current_dimensions every frame, and each change reconfigured the surface
     * AND destroyed+recreated the three full-res offscreen targets (scene, depth,
     * post) — pure churn for a size that is about to change again next frame.
     * Instead, hold the current committed size until the requested size has been
     * stable for WGPU_RESIZE_STABLE_FRAMES consecutive frames, then apply it once.
     *
     * Why this variant cannot produce a visible error/black frame: the surface and
     * all three offscreen targets are ALWAYS the same committed size (they only ever
     * change together, atomically, when we commit) — so the end-frame present copy's
     * extent {s_scene_w, s_scene_h} always exactly matches both the source target and
     * the surface destination, and can never over-run. While the window outgrows the
     * held surface, wgpuSurfaceGetCurrentTexture returns SuccessSuboptimal (already
     * accepted as present_ok) and the compositor scales — no error, no black frame.
     * The transient during an active drag is at most a briefly-scaled / viewport-
     * clamped image for a few frames, which snaps crisp the instant the size settles.
     *
     * Cannot wedge: this runs every frame, and any size held for STABLE_FRAMES
     * commits — so the final size after a drag (held indefinitely once the mouse is
     * released) always applies. The first size ever seen (s_cfg_w == 0) and any size
     * already live (req == committed) apply immediately, so a fixed-resolution run
     * (every headless gate) never debounces and stays byte-identical.
     *
     * Web note: the browser canvas resizes are rare and ResizeObserver-driven (not a
     * per-frame drag), so in practice this only affects native drags; the mechanism is
     * identical and harmless on web (a rare canvas resize applies within a few frames). */
    #define WGPU_RESIZE_STABLE_FRAMES 3
    uint32_t rw, rh;
    if (s_cfg_w == 0 || s_cfg_h == 0 || s_scene_w == 0 || s_scene_h == 0 ||
        (req_w == s_cfg_w && req_h == s_cfg_h)) {
        /* Initial bring-up, or the requested size is already live: apply now. */
        rw = req_w; rh = req_h;
        s_resize_pending_w = req_w; s_resize_pending_h = req_h;
        s_resize_stable = 0;
    } else {
        /* Requested size differs from the committed one: debounce. */
        if (req_w == s_resize_pending_w && req_h == s_resize_pending_h) {
            s_resize_stable++;
        } else {
            s_resize_pending_w = req_w; s_resize_pending_h = req_h;
            s_resize_stable = 1;
        }
        if (s_resize_stable >= WGPU_RESIZE_STABLE_FRAMES) {
            rw = req_w; rh = req_h;   /* stable long enough — commit the new size */
            s_resize_stable = 0;
        } else {
            rw = s_cfg_w; rh = s_cfg_h;   /* keep rendering at the committed size */
        }
    }

    /*
     * The surface is the WINDOW, so it follows the OUTPUT size. The scene
     * target below follows the RENDER size (output x Video.RenderScale) and
     * wgpu_run_resolve() bridges the two at present. Configuring the surface
     * from the scaled size instead would make the swapchain itself
     * supersampled, which is not what a swapchain is for and would leave the
     * present copy matching by accident.
     */
    /* Scene/depth/post render at OUTPUT x Video.RenderScale, clamped to the
     * device's offscreen limit. Derived from the committed output size so a
     * debounced resize can never leave the two disagreeing. */
    uint32_t out_w = rw, out_h = rh;
    {
        uint32_t sw;
        uint32_t sh;
        gfx_render_scaled_dimensions(
            rw, rh, s_max_tex_dim, &sw, &sh);
        rw = sw;
        rh = sh;
    }
    /* Reset the SSAO scene-projection coefficients each frame (mirrors
     * gfx_opengl.c:4182 and gfx_metal.mm:2036): the largest-far projection seen
     * during this frame's draws (gfx_pc.c:16128) wins. Without the reset the
     * `proj_b != 0` SSAO gate would latch on forever and menu/HUD frames that set
     * no projection would still get AO. proj_a is set in lockstep with proj_b, so
     * only proj_b/x/y are cleared — exactly as GL/Metal do. */
    g_pc_ssao_proj_b = 0.0f;
    g_pc_ssao_proj_x = 0.0f;
    g_pc_ssao_proj_y = 0.0f;
    /* (Re)create the offscreen scene target + depth buffer at the render res. */
    if (s_scene_view == NULL || s_scene_w != rw || s_scene_h != rh) {
        WGPUTexture new_scene_tex = NULL;
        WGPUTextureView new_scene_view = NULL;
        WGPUTexture new_depth_tex = NULL;
        WGPUTextureView new_depth_view = NULL;
        WGPUTexture new_post_tex = NULL;
        WGPUTextureView new_post_view = NULL;
        WGPUTextureDescriptor td = {0};
        td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc |
                   WGPUTextureUsage_TextureBinding;
        td.dimension = WGPUTextureDimension_2D;
        td.size.width = rw; td.size.height = rh; td.size.depthOrArrayLayers = 1;
        td.format = s_surface_format;   /* BGRA8 — matches the surface for the present copy */
        td.mipLevelCount = 1; td.sampleCount = 1;
        new_scene_tex = WGPU_FAULT_CREATE(
            SCENE_TEXTURE, wgpuDeviceCreateTexture(s_device, &td));
        new_scene_view = new_scene_tex
            ? WGPU_FAULT_CREATE(
                SCENE_VIEW, wgpuTextureCreateView(new_scene_tex, NULL))
            : NULL;

        WGPUTextureDescriptor dd = {0};
        /* TextureBinding: the SSAO post-FX pass samples this depth target as a
         * texture_depth_2d (default-off; inert when Video.Ssao=0). Adding the usage
         * flag does not change any rendered pixel — the scene pass output is
         * unaffected, so faithful frames stay byte-identical. */
        dd.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
        dd.dimension = WGPUTextureDimension_2D;
        dd.size.width = rw; dd.size.height = rh; dd.size.depthOrArrayLayers = 1;
        dd.format = WGPU_DEPTH_FORMAT;
        dd.mipLevelCount = 1; dd.sampleCount = 1;
        new_depth_tex = WGPU_FAULT_CREATE(
            DEPTH_TEXTURE, wgpuDeviceCreateTexture(s_device, &dd));
        new_depth_view = new_depth_tex
            ? WGPU_FAULT_CREATE(
                DEPTH_VIEW, wgpuTextureCreateView(new_depth_tex, NULL))
            : NULL;

        /* Output post-FX target (same format/size as the scene). RenderAttachment
         * so the filter pass writes it; CopySrc so present/readback/dump copy it. */
        WGPUTextureDescriptor pt = {0};
        pt.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc |
                   WGPUTextureUsage_TextureBinding;
        pt.dimension = WGPUTextureDimension_2D;
        pt.size.width = rw; pt.size.height = rh; pt.size.depthOrArrayLayers = 1;
        pt.format = s_surface_format;
        pt.mipLevelCount = 1; pt.sampleCount = 1;
        new_post_tex = WGPU_FAULT_CREATE(
            POST_TEXTURE, wgpuDeviceCreateTexture(s_device, &pt));
        new_post_view = new_post_tex
            ? WGPU_FAULT_CREATE(
                POST_VIEW, wgpuTextureCreateView(new_post_tex, NULL))
            : NULL;

        if (new_scene_view == NULL || new_depth_view == NULL ||
            new_post_view == NULL ||
            s_runtime_status == GFX_RENDERING_FATAL) {
            if (new_scene_view != NULL) wgpuTextureViewRelease(new_scene_view);
            if (new_scene_tex != NULL) wgpuTextureRelease(new_scene_tex);
            if (new_depth_view != NULL) wgpuTextureViewRelease(new_depth_view);
            if (new_depth_tex != NULL) wgpuTextureRelease(new_depth_tex);
            if (new_post_view != NULL) wgpuTextureViewRelease(new_post_view);
            if (new_post_tex != NULL) wgpuTextureRelease(new_post_tex);
            fprintf(stderr,
                    "[webgpu] transactional scene-target allocation failed "
                    "for %ux%u\n", rw, rh);
            s_ready = false;
            s_runtime_status = GFX_RENDERING_FATAL;
            WGPU_COMPAT_REPORT_FAILURE(
                "The graphics backend could not allocate render targets. "
                "Reload the page to continue from the last persisted save.");
            return false;
        }
        if (s_scene_view != NULL) wgpuTextureViewRelease(s_scene_view);
        if (s_scene_tex != NULL) wgpuTextureRelease(s_scene_tex);
        if (s_depth_view != NULL) wgpuTextureViewRelease(s_depth_view);
        if (s_depth_tex != NULL) wgpuTextureRelease(s_depth_tex);
        if (s_post_view != NULL) wgpuTextureViewRelease(s_post_view);
        if (s_post_tex != NULL) wgpuTextureRelease(s_post_tex);
        s_scene_tex = new_scene_tex;
        s_scene_view = new_scene_view;
        s_depth_tex = new_depth_tex;
        s_depth_view = new_depth_view;
        s_post_tex = new_post_tex;
        s_post_view = new_post_view;
        s_scene_w = rw; s_scene_h = rh;
    }

    /*
     * Output-sized resolve target. Persistent and CopySrc so the present copy,
     * readback and the frame dumper all read OUTPUT-sized pixels — if the
     * scaled scene reached them, every pixel-comparison gate in the project
     * would silently start scoring supersampled frames.
     */
    {
        if (s_resolve_view == NULL || s_output_depth_view == NULL ||
            s_resolve_w != out_w || s_resolve_h != out_h) {
            WGPUTexture new_resolve_tex;
            WGPUTextureView new_resolve_view;
            WGPUTexture new_output_depth_tex;
            WGPUTextureView new_output_depth_view;
            WGPUTextureDescriptor rt = {0};
            rt.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc |
                       WGPUTextureUsage_TextureBinding;
            rt.dimension = WGPUTextureDimension_2D;
            rt.size.width = out_w; rt.size.height = out_h; rt.size.depthOrArrayLayers = 1;
            rt.format = s_surface_format;
            rt.mipLevelCount = 1; rt.sampleCount = 1;
            new_resolve_tex = WGPU_FAULT_CREATE(
                RESOLVE_TEXTURE, wgpuDeviceCreateTexture(s_device, &rt));
            new_resolve_view = new_resolve_tex
                ? WGPU_FAULT_CREATE(
                    RESOLVE_VIEW,
                    wgpuTextureCreateView(new_resolve_tex, NULL))
                : NULL;
            WGPUTextureDescriptor od = {0};
            od.usage = WGPUTextureUsage_RenderAttachment;
            od.dimension = WGPUTextureDimension_2D;
            od.size.width = out_w;
            od.size.height = out_h;
            od.size.depthOrArrayLayers = 1;
            od.format = WGPU_DEPTH_FORMAT;
            od.mipLevelCount = 1;
            od.sampleCount = 1;
            new_output_depth_tex = WGPU_FAULT_CREATE(
                OUTPUT_DEPTH_TEXTURE,
                wgpuDeviceCreateTexture(s_device, &od));
            new_output_depth_view = new_output_depth_tex
                ? WGPU_FAULT_CREATE(
                    OUTPUT_DEPTH_VIEW,
                    wgpuTextureCreateView(new_output_depth_tex, NULL))
                : NULL;
            if (new_resolve_view == NULL || new_output_depth_view == NULL ||
                s_runtime_status == GFX_RENDERING_FATAL) {
                if (new_resolve_view != NULL) {
                    wgpuTextureViewRelease(new_resolve_view);
                }
                if (new_resolve_tex != NULL) {
                    wgpuTextureRelease(new_resolve_tex);
                }
                if (new_output_depth_view != NULL) {
                    wgpuTextureViewRelease(new_output_depth_view);
                }
                if (new_output_depth_tex != NULL) {
                    wgpuTextureRelease(new_output_depth_tex);
                }
                fprintf(stderr,
                        "[webgpu] transactional resolve-target allocation "
                        "failed for %ux%u\n", out_w, out_h);
                s_ready = false;
                s_runtime_status = GFX_RENDERING_FATAL;
                WGPU_COMPAT_REPORT_FAILURE(
                    "The graphics backend could not allocate a resolve target. "
                    "Reload the page to continue from the last persisted save.");
                return false;
            }
            if (s_resolve_view != NULL) {
                wgpuTextureViewRelease(s_resolve_view);
            }
            if (s_resolve_tex != NULL) {
                wgpuTextureRelease(s_resolve_tex);
            }
            if (s_output_depth_view != NULL) {
                wgpuTextureViewRelease(s_output_depth_view);
            }
            if (s_output_depth_tex != NULL) {
                wgpuTextureRelease(s_output_depth_tex);
            }
            s_resolve_tex = new_resolve_tex;
            s_resolve_view = new_resolve_view;
            s_output_depth_tex = new_output_depth_tex;
            s_output_depth_view = new_output_depth_view;
            s_resolve_w = out_w; s_resolve_h = out_h;
        }
    }
    /*
     * Only configure/commit the surface after every size-coupled offscreen
     * resource exists. This prevents a resize from publishing dimensions that
     * no complete render-target set can satisfy.
     */
    if ((out_w != s_cfg_w || out_h != s_cfg_h ||
         s_cfg_present_mode_dirty) &&
        !wgpu_configure_surface(out_w, out_h)) {
        fprintf(stderr,
                "[webgpu] surface configuration failed for %ux%u\n",
                out_w, out_h);
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics surface could not be configured. Reload the page to "
            "continue from the last persisted save.");
        return false;
    }
    if (s_scene_view == NULL || s_depth_view == NULL) {
        return false;
    }

    /* WEB-027: advance + upload the per-frame noise uniform now that s_scene_h is
     * established, before any draw builds a bind group that references it. */
    wgpu_update_noise_ubo();
    wgpu_update_light_ubo();

    /* WEB-053: every draw recorded from here on belongs to a new encoder. A
     * texture last drawn under an earlier epoch has had its command buffer
     * finished (submitted or discarded), so overwriting its texels in place can
     * no longer rewrite a pending draw. See wgpu_upload_texture. */
    s_draw_epoch++;
    if (s_draw_epoch == 0) {
        s_draw_epoch = 1;   /* 0 is the never-drawn sentinel */
    }
    s_encoder = WGPU_FAULT_CREATE(
        FRAME_ENCODER, wgpuDeviceCreateCommandEncoder(s_device, NULL));
    if (s_encoder == NULL) {
        fprintf(stderr, "[webgpu] command encoder creation failed\n");
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend could not allocate a frame. Reload the page "
            "to continue from the last persisted save.");
        return false;
    }

    /* Replay the previous immutable caster frame before the ordinary scene
     * pass opens. This is a second GPU pass, never a second game/DL traversal. */
    wgpu_render_shadow_maps();

    WGPURenderPassColorAttachment att = {0};
    att.view = s_scene_view;
    att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;   /* required for a 2D color target */
    att.loadOp = WGPULoadOp_Clear;
    att.storeOp = WGPUStoreOp_Store;
    att.clearValue.r = s_clear_r;
    att.clearValue.g = s_clear_g;
    att.clearValue.b = s_clear_b;
    att.clearValue.a = 1.0;
    WGPURenderPassDepthStencilAttachment depth = {0};
    depth.view = s_depth_view;
    depth.depthLoadOp = WGPULoadOp_Clear;
    depth.depthStoreOp = WGPUStoreOp_Store;
    depth.depthClearValue = 1.0f;   /* 1.0 = far, with WebGPU's 0..1 clip (0 = near) */

    WGPURenderPassDescriptor rp = {0};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;
    rp.depthStencilAttachment = &depth;
    s_pass = WGPU_FAULT_CREATE(
        FRAME_PASS, wgpuCommandEncoderBeginRenderPass(s_encoder, &rp));
    if (s_pass == NULL) {
        wgpuCommandEncoderRelease(s_encoder);
        s_encoder = NULL;
        fprintf(stderr,
                "[webgpu] command encoder/render pass creation failed\n");
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend could not allocate a frame. Reload the page "
            "to continue from the last persisted save.");
        return false;
    }
    s_frame_open = true;
    return true;
}

typedef struct { int done; WGPUMapAsyncStatus status; } WgpuMapReq;
static void on_map(WGPUMapAsyncStatus s, WGPUStringView m, void *u1, void *u2) {
    (void)m; (void)u2; WgpuMapReq *r = (WgpuMapReq *)u1; r->status = s; r->done = 1;
}

/* GE007_WEBGPU_DUMP_FRAME=<n> writes presented frame n to a PPM (debug/validation
 * seed for read_framebuffer_rgb). Returns the target frame, or -1 if unset. */
static int wgpu_dump_target_frame(void) {
    static int cached = -2;
    if (cached == -2) {
        const char *e = getenv("GE007_WEBGPU_DUMP_FRAME");
        cached = e ? atoi(e) : -1;
    }
    return cached;
}

/* GE007_WEBGPU_DUMP_SURFACE=<n> writes the PRESENTED surface (scene + F1 overlay)
 * for frame n to a PPM — the scene dump above is overlay-free (pre-blit). */
static int wgpu_dump_surface_frame(void) {
    static int cached = -2;
    if (cached == -2) {
        const char *e = getenv("GE007_WEBGPU_DUMP_SURFACE");
        cached = e ? atoi(e) : -1;
    }
    return cached;
}

/* Map `buf` (bytesPerRow=bpr, BGRA8) and write a w*h RGB PPM to `path`. */
static void wgpu_write_ppm(WGPUBuffer buf, uint32_t bpr, uint32_t w, uint32_t h, const char *path) {
    size_t size = (size_t)bpr * h;
    /* WEB-026: static so a timed-out map's late-resolving callback lands on live
     * storage, not a dead stack frame. Single-threaded + synchronous, so no two
     * maps are ever in flight; reset per call. */
    static WgpuMapReq mr;
    mr = (WgpuMapReq){0};
    WGPUBufferMapCallbackInfo ci = {0};
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = on_map;
    ci.userdata1 = &mr;
    if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_READBACK_MAP)) {
        mr.done = 1;
        mr.status = WGPUMapAsyncStatus_Error;
    } else {
        wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, size, ci);
    }
    /* WEB-004: pass s_instance (not NULL) so the browser pump can drive
     * wgpuInstanceProcessEvents — the mapAsync callback ONLY fires during
     * ProcessEvents, so a NULL instance froze the tab for minutes on web. On
     * native the WAIT macro prefers the (non-NULL) device and calls
     * wgpuDevicePoll exactly as before — byte-identical. */
    WGPU_PIPELINE_CALLBACK_OWNER_WAIT(
        mr.done, s_instance, s_device, 100000);
    if (!mr.done || mr.status != WGPUMapAsyncStatus_Success) {
        fprintf(stderr, "[webgpu] frame dump map failed (status=%d)\n", (int)mr.status);
        return;
    }
    const uint8_t *px = (const uint8_t *)wgpuBufferGetConstMappedRange(buf, 0, size);
    const bool bgra = wgpu_format_is_bgra(s_surface_format);
    FILE *f = px ? mdkr_fopen_utf8(path, "wb") : NULL;
    if (f != NULL) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *row = px + (size_t)y * bpr;
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *p = row + (size_t)x * 4;   /* BGRA8 (or RGBA8) */
                uint8_t rgb[3] = { bgra ? p[2] : p[0], p[1], bgra ? p[0] : p[2] };
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
        fprintf(stderr, "[webgpu] wrote frame dump %s (%ux%u)\n", path, w, h);
    }
    wgpuBufferUnmap(buf);
}

/* ------------------------------------------------------------------------
 * Output-VI-filter post-FX pass (FXAA / linear-light world finish / gamma /
 * vignette / CAS sharpen / dither / RGB555). A fullscreen-triangle pass that
 * resolves s_scene_tex -> s_post_tex, faithfully porting gfx_opengl.c's output
 * filter (see gfx_webgpu_postfx_wgsl). Gating mirrors GL exactly: uApplyPost ==
 * g_pcRemasterFX, each effect further gated on its own g_pc* setting. SSAO
 * (planar v1) reads the sampleable scene depth target (default-off; Video.Ssao).
 * ---------------------------------------------------------------------- */
typedef struct {
    float srcSize[2];
    float dstSize[2];
    float colorScale, colorBias, gamma, saturation;
    float contrast, brightness, vignette, sharpen;
    float bloomThreshold, bloomIntensity, levelSat, levelCon;
    float colorTint[3];
    int32_t applyPost;
    int32_t dither, bloom, fxaa;
    int32_t tonemap, rgb555, linearFinish, ssao;
    /* SSAO (planar v1) — ports gfx_opengl.c's uSsao* uniforms. */
    float ssaoRadius, ssaoIntensity, ssaoAspect, ssaoProjA;
    float ssaoProjB;   /* struct ends at 128 bytes — WGSL 16-byte-rounded, no trailing pad */
} WgpuPostU;

static WGPURenderPipeline  s_post_pipe = NULL;
static WGPUBindGroupLayout s_post_bgl  = NULL;
static WGPUBuffer          s_post_ubuf = NULL;
static WGPUSampler         s_post_sampN = NULL;   /* nearest + clamp */
static WGPUSampler         s_post_sampL = NULL;   /* linear + clamp */
static WGPUBindGroup       s_post_bg = NULL;
static WGPUTextureView     s_post_bg_view = NULL; /* scene view the bind group binds */

static float wgpu_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* True when the output filter changes any pixel — i.e. the master remaster
 * switch is on, or a non-identity display gamma is set. When false, end_frame
 * keeps the plain scene->surface copy so faithful frames stay byte-identical to
 * the pre-post-FX backend (and the tape/aperture gates). Mirrors the spirit of
 * gfx_opengl_output_color_adjust_active (the diag colorScale/bias/rgb555 knobs
 * are GL-file statics, not ported — they are off by default). */
static bool wgpu_postfx_active(void) {
    float gamma = wgpu_clampf(g_pcVideoGamma, 0.5f, 2.5f);
    if (gamma < 0.999f || gamma > 1.001f) {
        return true;
    }
    return g_pcRemasterFX != 0;
}

/* ==========================================================================
 *  Supersample resolve (mdkr64)
 *
 *  Video.RenderScale renders the scene at drawable x scale. The surface stays
 *  at drawable size, so the present copy -- which is a texture-to-texture copy
 *  and therefore requires matching extents -- cannot see the scaled scene. This
 *  pass is the single place the frame returns from render space to output
 *  space.
 *
 *  It is a real NxN box, not a bilinear shrink. Bilinear happens to be an exact
 *  box at 2x (each output pixel centre lands on a texel boundary, so the four
 *  neighbours weigh 1/4 each) but at 3x or 4x it reads only 4 of the 9 or 16
 *  covered texels and throws the rest away -- which would quietly make "4x
 *  supersampling" worse than 2x on thin geometry.
 *
 *  Resolving into a persistent output-sized texture rather than straight to the
 *  surface is deliberate: readback and the frame dumper copy from it, so they
 *  keep returning OUTPUT-sized frames. If the scaled scene reached them, every
 *  pixel-comparison gate in the project would silently start scoring
 *  supersampled images.
 * ========================================================================== */

typedef struct WgpuResolveU {
    float srcSize[2];
    float dstSize[2];
    int32_t taps;       /* box is taps x taps source texels */
    int32_t _pad[3];
} WgpuResolveU;

static const char *WGPU_RESOLVE_WGSL =
    "struct RU { srcSize : vec2<f32>, dstSize : vec2<f32>, taps : i32, _p0 : i32, _p1 : i32, _p2 : i32 };\n"
    "@group(0) @binding(0) var<uniform> u : RU;\n"
    "@group(0) @binding(1) var uTex : texture_2d<f32>;\n"
    "@group(0) @binding(2) var uSamp : sampler;\n"
    "struct VOut { @builtin(position) pos : vec4<f32> };\n"
    "@vertex fn vs_main(@builtin(vertex_index) vid : u32) -> VOut {\n"
    "  var p = array<vec2<f32>,3>(vec2<f32>(-1.0,-3.0), vec2<f32>(-1.0,1.0), vec2<f32>(3.0,1.0));\n"
    "  var o : VOut;\n"
    "  o.pos = vec4<f32>(p[vid], 0.0, 1.0);\n"
    "  return o;\n"
    "}\n"
    "@fragment fn fs_main(in : VOut) -> @location(0) vec4<f32> {\n"
    "  let dst = floor(in.pos.xy);\n"
    "  let ratio = u.srcSize / u.dstSize;\n"
    "  let origin = dst * ratio;\n"
    "  var acc = vec4<f32>(0.0);\n"
    "  var wsum = 0.0;\n"
    "  for (var y : i32 = 0; y < u.taps; y = y + 1) {\n"
    "    for (var x : i32 = 0; x < u.taps; x = x + 1) {\n"
    "      let off = vec2<f32>(f32(x) + 0.5, f32(y) + 0.5);\n"
    "      let sp = origin + off;\n"
    "      if (sp.x < u.srcSize.x && sp.y < u.srcSize.y) {\n"
    "        acc = acc + textureSampleLevel(uTex, uSamp, sp / u.srcSize, 0.0);\n"
    "        wsum = wsum + 1.0;\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  if (wsum <= 0.0) {\n"
    "    return textureSampleLevel(uTex, uSamp, (dst + vec2<f32>(0.5)) / u.dstSize, 0.0);\n"
    "  }\n"
    "  return acc / wsum;\n"
    "}\n";

static bool wgpu_ensure_resolve(void) {
    WGPUShaderModule mod = NULL;
    WGPUBindGroupLayout bgl = NULL;
    WGPUBuffer ubuf = NULL;
    WGPUSampler sampler = NULL;
    WGPUPipelineLayout layout = NULL;
    WGPURenderPipeline pipeline = NULL;

    if (s_resolve_pipe != NULL) {
        return true;
    }
    if (!s_ready) {
        return false;
    }
    WGPUShaderSourceWGSL src = {0};
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = wgpu_sv(WGPU_RESOLVE_WGSL);
    WGPUShaderModuleDescriptor smd = {0};
    smd.nextInChain = (WGPUChainedStruct *)&src;
    mod = WGPU_FAULT_CREATE(
        RESOLVE_MODULE, wgpuDeviceCreateShaderModule(s_device, &smd));
    if (mod == NULL) {
        goto fail;
    }

    WGPUBindGroupLayoutEntry e[3] = {0};
    e[0].binding = 0; e[0].visibility = WGPUShaderStage_Fragment;
    e[0].buffer.type = WGPUBufferBindingType_Uniform;
    e[0].buffer.minBindingSize = sizeof(WgpuResolveU);
    e[1].binding = 1; e[1].visibility = WGPUShaderStage_Fragment;
    e[1].texture.sampleType = WGPUTextureSampleType_Float;
    e[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    e[2].binding = 2; e[2].visibility = WGPUShaderStage_Fragment;
    e[2].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor bgld = {0};
    bgld.entryCount = 3; bgld.entries = e;
    bgl = WGPU_FAULT_CREATE(
        RESOLVE_BGL, wgpuDeviceCreateBindGroupLayout(s_device, &bgld));

    WGPUBufferDescriptor ubd = {0};
    ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubd.size = sizeof(WgpuResolveU);
    ubuf = WGPU_FAULT_CREATE(
        RESOLVE_UNIFORM, wgpuDeviceCreateBuffer(s_device, &ubd));

    WGPUSamplerDescriptor sd = {0};
    sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_ClampToEdge;
    sd.magFilter = sd.minFilter = WGPUFilterMode_Linear;
    sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sd.maxAnisotropy = 1;
    sampler = WGPU_FAULT_CREATE(
        RESOLVE_SAMPLER, wgpuDeviceCreateSampler(s_device, &sd));
    if (bgl == NULL || ubuf == NULL || sampler == NULL) {
        goto fail;
    }

    WGPUPipelineLayoutDescriptor pld = {0};
    pld.bindGroupLayoutCount = 1; pld.bindGroupLayouts = &bgl;
    layout = WGPU_FAULT_CREATE(
        RESOLVE_LAYOUT, wgpuDeviceCreatePipelineLayout(s_device, &pld));
    if (layout == NULL) {
        goto fail;
    }

    WGPUColorTargetState color = {0};
    color.format = s_surface_format;
    color.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs = {0};
    fs.module = mod; fs.entryPoint = wgpu_sv("fs_main");
    fs.targetCount = 1; fs.targets = &color;
    WGPURenderPipelineDescriptor pd = {0};
    pd.layout = layout;
    pd.vertex.module = mod; pd.vertex.entryPoint = wgpu_sv("vs_main");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    pipeline = WGPU_FAULT_CREATE(
        RESOLVE_PIPELINE, wgpuDeviceCreateRenderPipeline(s_device, &pd));
    if (pipeline == NULL) {
        goto fail;
    }

    s_resolve_bgl = bgl;
    s_resolve_ubuf = ubuf;
    s_resolve_samp = sampler;
    s_resolve_pipe = pipeline;
    wgpuPipelineLayoutRelease(layout);
    wgpuShaderModuleRelease(mod);
    return true;

fail:
    if (pipeline != NULL) wgpuRenderPipelineRelease(pipeline);
    if (layout != NULL) wgpuPipelineLayoutRelease(layout);
    if (sampler != NULL) wgpuSamplerRelease(sampler);
    if (ubuf != NULL) wgpuBufferRelease(ubuf);
    if (bgl != NULL) wgpuBindGroupLayoutRelease(bgl);
    if (mod != NULL) wgpuShaderModuleRelease(mod);
    fprintf(stderr, "[webgpu] transactional resolve-resource creation failed\n");
    wgpu_runtime_fatal(
        "The graphics backend could not allocate its resolve pipeline. Reload "
        "the page to continue from the last persisted save.");
    return false;
}

/* Blit `src` into an arbitrary same-format render attachment. */
/* Hand out this frame's next unwritten resolve uniform buffer, growing the ring
 * rather than reusing an occupied slot (see the ring note above). */
static WGPUBuffer wgpu_resolve_ubo_next(void) {
    int index = s_resolve_ubo_used;
    if (index == 0) {
        s_resolve_ubo_used = 1;
        return s_resolve_ubuf;
    }
    index -= 1;
    if (index >= s_resolve_ubo_ext_cap) {
        int ncap = s_resolve_ubo_ext_cap ? s_resolve_ubo_ext_cap * 2 : 4;
        WGPUBuffer *grown = (WGPUBuffer *)realloc(
            s_resolve_ubo_ext, (size_t)ncap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        for (int i = s_resolve_ubo_ext_cap; i < ncap; i++) grown[i] = NULL;
        s_resolve_ubo_ext = grown;
        s_resolve_ubo_ext_cap = ncap;
    }
    if (s_resolve_ubo_ext[index] == NULL) {
        WGPUBufferDescriptor bd = {0};
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bd.size = sizeof(WgpuResolveU);
        s_resolve_ubo_ext[index] = WGPU_FAULT_CREATE(
            RESOLVE_UNIFORM, wgpuDeviceCreateBuffer(s_device, &bd));
        if (s_resolve_ubo_ext[index] == NULL) {
            return NULL;
        }
    }
    s_resolve_ubo_used++;
    return s_resolve_ubo_ext[index];
}

static bool wgpu_run_resolve_to(WGPUCommandEncoder enc,
                                WGPUTextureView src, uint32_t src_w,
                                uint32_t src_h, WGPUTextureView destination,
                                uint32_t dst_w, uint32_t dst_h) {
    if (!wgpu_ensure_resolve() || enc == NULL || src == NULL ||
        destination == NULL || src_w == 0 || src_h == 0 ||
        dst_w == 0 || dst_h == 0) {
        return false;
    }
    WGPUBuffer ubuf = wgpu_resolve_ubo_next();
    if (ubuf == NULL) {
        fprintf(stderr, "[webgpu] resolve uniform allocation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not allocate a resolve uniform. "
            "Reload the page to continue from the last persisted save.");
        return false;
    }
    {
        WgpuResolveU u = {0};
        int taps;
        u.srcSize[0] = (float)src_w; u.srcSize[1] = (float)src_h;
        u.dstSize[0] = (float)dst_w; u.dstSize[1] = (float)dst_h;
        taps = (int)((src_w + dst_w - 1u) / dst_w);
        if (taps < 1) taps = 1;
        if (taps > 8) taps = 8;      /* bounds the inner loop; scale is clamped to 4 */
        u.taps = taps;
        wgpuQueueWriteBuffer(s_queue, ubuf, 0, &u, sizeof(u));
    }
    {
        WGPUBindGroupEntry be[3] = {0};
        be[0].binding = 0; be[0].buffer = ubuf; be[0].size = sizeof(WgpuResolveU);
        be[1].binding = 1; be[1].textureView = src;
        be[2].binding = 2; be[2].sampler = s_resolve_samp;
        WGPUBindGroupDescriptor bgd = {0};
        bgd.layout = s_resolve_bgl; bgd.entryCount = 3; bgd.entries = be;
        WGPUBindGroup bg = WGPU_FAULT_CREATE(
            RESOLVE_BIND_GROUP,
            wgpuDeviceCreateBindGroup(s_device, &bgd));
        if (bg == NULL) {
            fprintf(stderr, "[webgpu] resolve bind-group creation failed\n");
            wgpu_runtime_fatal(
                "The graphics backend could not allocate a resolve binding. "
                "Reload the page to continue from the last persisted save.");
            return false;
        }
        {
            WGPURenderPassColorAttachment att = {0};
            WGPURenderPassDescriptor rp = {0};
            WGPURenderPassEncoder pass;
            att.view = destination;
            att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            att.loadOp = WGPULoadOp_Clear;
            att.storeOp = WGPUStoreOp_Store;
            rp.colorAttachmentCount = 1; rp.colorAttachments = &att;
            pass = WGPU_FAULT_CREATE(
                RESOLVE_PASS,
                wgpuCommandEncoderBeginRenderPass(enc, &rp));
            if (pass == NULL) {
                wgpuBindGroupRelease(bg);
                fprintf(stderr, "[webgpu] resolve-pass creation failed\n");
                wgpu_runtime_fatal(
                    "The graphics backend could not begin a resolve pass. "
                    "Reload the page to continue from the last persisted save.");
                return false;
            }
            wgpuRenderPassEncoderSetPipeline(pass, s_resolve_pipe);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(pass);
            wgpuRenderPassEncoderRelease(pass);
        }
        wgpuBindGroupRelease(bg);
    }
    return true;
}

/* Blit `src` (render resolution) into s_resolve_tex (output resolution). */
static bool wgpu_run_resolve(WGPUCommandEncoder enc,
                             WGPUTextureView src, uint32_t src_w,
                             uint32_t src_h, uint32_t dst_w,
                             uint32_t dst_h) {
    return wgpu_run_resolve_to(enc, src, src_w, src_h, s_resolve_view,
                               dst_w, dst_h);
}

static bool wgpu_ensure_postfx(void) {
    WGPUShaderModule mod = NULL;
    WGPUBindGroupLayout bgl = NULL;
    WGPUBuffer ubuf = NULL;
    WGPUSampler sampler_nearest = NULL;
    WGPUSampler sampler_linear = NULL;
    WGPUPipelineLayout layout = NULL;
    WGPURenderPipeline pipeline = NULL;

    if (s_post_pipe != NULL) {
        return true;
    }
    if (!s_ready) {
        return false;
    }
    WGPUShaderSourceWGSL src = {0};
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = wgpu_sv(gfx_webgpu_postfx_wgsl());
    WGPUShaderModuleDescriptor smd = {0};
    smd.nextInChain = (WGPUChainedStruct *)&src;
    mod = WGPU_FAULT_CREATE(
        POST_MODULE, wgpuDeviceCreateShaderModule(s_device, &smd));
    if (mod == NULL) {
        goto fail;
    }

    /* group 0: uniform(0), scene texture(1), nearest sampler(2), linear sampler(3),
     * scene depth texture(4, for SSAO), non-filtering depth sampler(5). */
    WGPUBindGroupLayoutEntry e[6] = {0};
    e[0].binding = 0; e[0].visibility = WGPUShaderStage_Fragment;
    e[0].buffer.type = WGPUBufferBindingType_Uniform;
    e[0].buffer.minBindingSize = sizeof(WgpuPostU);
    e[1].binding = 1; e[1].visibility = WGPUShaderStage_Fragment;
    e[1].texture.sampleType = WGPUTextureSampleType_Float;
    e[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    e[2].binding = 2; e[2].visibility = WGPUShaderStage_Fragment;
    e[2].sampler.type = WGPUSamplerBindingType_Filtering;
    e[3].binding = 3; e[3].visibility = WGPUShaderStage_Fragment;
    e[3].sampler.type = WGPUSamplerBindingType_Filtering;
    /* SSAO depth read: depth textures are unfilterable — sampleType Depth + a
     * NonFiltering sampler (nearest/clamp, matching GL/Metal's depth sampler). */
    e[4].binding = 4; e[4].visibility = WGPUShaderStage_Fragment;
    e[4].texture.sampleType = WGPUTextureSampleType_Depth;
    e[4].texture.viewDimension = WGPUTextureViewDimension_2D;
    e[5].binding = 5; e[5].visibility = WGPUShaderStage_Fragment;
    e[5].sampler.type = WGPUSamplerBindingType_NonFiltering;
    WGPUBindGroupLayoutDescriptor bgld = {0};
    bgld.entryCount = 6;
    bgld.entries = e;
    bgl = WGPU_FAULT_CREATE(
        POST_BGL, wgpuDeviceCreateBindGroupLayout(s_device, &bgld));

    WGPUBufferDescriptor ubd = {0};
    ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubd.size = sizeof(WgpuPostU);
    ubuf = WGPU_FAULT_CREATE(
        POST_UNIFORM, wgpuDeviceCreateBuffer(s_device, &ubd));

    WGPUSamplerDescriptor sn = {0};
    sn.addressModeU = sn.addressModeV = sn.addressModeW = WGPUAddressMode_ClampToEdge;
    sn.magFilter = sn.minFilter = WGPUFilterMode_Nearest;
    sn.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sn.maxAnisotropy = 1;
    sampler_nearest = WGPU_FAULT_CREATE(
        POST_SAMPLER_NEAREST, wgpuDeviceCreateSampler(s_device, &sn));
    WGPUSamplerDescriptor sl = sn;
    sl.magFilter = sl.minFilter = WGPUFilterMode_Linear;
    sampler_linear = WGPU_FAULT_CREATE(
        POST_SAMPLER_LINEAR, wgpuDeviceCreateSampler(s_device, &sl));
    if (bgl == NULL || ubuf == NULL || sampler_nearest == NULL ||
        sampler_linear == NULL) {
        goto fail;
    }

    WGPUPipelineLayoutDescriptor pld = {0};
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &bgl;
    layout = WGPU_FAULT_CREATE(
        POST_LAYOUT, wgpuDeviceCreatePipelineLayout(s_device, &pld));
    if (layout == NULL) {
        goto fail;
    }

    WGPUColorTargetState color = {0};
    color.format = s_surface_format;
    color.writeMask = WGPUColorWriteMask_All;   /* opaque overwrite; no blend */
    WGPUFragmentState fs = {0};
    fs.module = mod; fs.entryPoint = wgpu_sv("fs_main");
    fs.targetCount = 1; fs.targets = &color;
    WGPURenderPipelineDescriptor pd = {0};
    pd.layout = layout;
    pd.vertex.module = mod; pd.vertex.entryPoint = wgpu_sv("vs_main");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.primitive.unclippedDepth = s_unclipped_depth_supported ? WGPU_TRUE : WGPU_FALSE;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;   /* no depth-stencil: fullscreen resolve has no depth */
    pipeline = WGPU_FAULT_CREATE(
        POST_PIPELINE, wgpuDeviceCreateRenderPipeline(s_device, &pd));
    if (pipeline == NULL) {
        goto fail;
    }

    s_post_bgl = bgl;
    s_post_ubuf = ubuf;
    s_post_sampN = sampler_nearest;
    s_post_sampL = sampler_linear;
    s_post_pipe = pipeline;
    wgpuPipelineLayoutRelease(layout);
    wgpuShaderModuleRelease(mod);
    return true;

fail:
    if (pipeline != NULL) wgpuRenderPipelineRelease(pipeline);
    if (layout != NULL) wgpuPipelineLayoutRelease(layout);
    if (sampler_linear != NULL) wgpuSamplerRelease(sampler_linear);
    if (sampler_nearest != NULL) wgpuSamplerRelease(sampler_nearest);
    if (ubuf != NULL) wgpuBufferRelease(ubuf);
    if (bgl != NULL) wgpuBindGroupLayoutRelease(bgl);
    if (mod != NULL) wgpuShaderModuleRelease(mod);
    fprintf(stderr, "[webgpu] transactional post-effect resource creation failed\n");
    wgpu_runtime_fatal(
        "The graphics backend could not allocate its post-processing pipeline. "
        "Reload the page to continue from the last persisted save.");
    return false;
}

/* Run the output filter: s_scene_tex -> `target`, using the still-open frame encoder.
 * `target` is s_post_view on the offscreen present path, or the acquired surface view
 * on the PERF-008 direct-to-surface path. Returns true if the target now holds the
 * filtered frame. */
static bool wgpu_run_postfx(WGPUTextureView target) {
    if (!wgpu_ensure_postfx() || s_encoder == NULL || s_scene_view == NULL ||
        target == NULL || s_scene_w == 0 || s_scene_h == 0) {
        return false;
    }

    int gp =
        (g_pcGradePresets && g_pcGradeLevelValid) ? 1 : 0;
    int apply_post = g_pcRemasterFX ? 1 : 0;
    WgpuPostU u = {0};
    u.srcSize[0] = (float)s_scene_w; u.srcSize[1] = (float)s_scene_h;
    u.dstSize[0] = (float)s_scene_w; u.dstSize[1] = (float)s_scene_h;
    u.colorScale = 1.0f;   /* GE007_DIAG_OUTPUT_FILTER_COLOR knobs not ported (diag-only) */
    u.colorBias  = 0.0f;
    u.gamma = wgpu_clampf(g_pcVideoGamma, 0.5f, 2.5f);
    u.saturation = wgpu_clampf(g_pcVideoSaturation, 0.0f, 2.0f);
    u.contrast   = wgpu_clampf(g_pcVideoContrast, 0.5f, 2.0f);
    u.brightness = wgpu_clampf(g_pcVideoBrightness, -0.5f, 0.5f);
    u.vignette   = wgpu_clampf(g_pcVignette, 0.0f, 1.0f);
    u.sharpen    = apply_post ? wgpu_clampf(g_pcSharpen, 0.0f, 1.0f) : 0.0f;
    u.bloomThreshold = g_pcBloomThreshold;
    u.bloomIntensity = g_pcBloomIntensity;
    u.levelSat = gp ? g_pcGradeLevelSat : 1.0f;
    u.levelCon = gp ? g_pcGradeLevelCon : 1.0f;
    u.colorTint[0] = gp ? g_pcGradeLevelTintR : 1.0f;
    u.colorTint[1] = gp ? g_pcGradeLevelTintG : 1.0f;
    u.colorTint[2] = gp ? g_pcGradeLevelTintB : 1.0f;
    u.applyPost = apply_post;
    u.dither = g_pcOutputDither ? 1 : 0;
    u.bloom = g_pcBloom ? 1 : 0;
    u.fxaa = (apply_post && g_pcFxaa) ? 1 : 0;
    u.tonemap =
        (apply_post && g_pcTonemap && g_pcGradeLevelValid) ? 1 : 0;
    u.rgb555 = 0;   /* GE007_DIAG_OUTPUT_RGB555 not ported (diag-only, default off) */
    /* SSAO (planar v1) — gate + uniforms mirror gfx_opengl.c:3561-3579 exactly.
     * apply_post already == g_pcRemasterFX (the remaster-master gate), so SSAO is a
     * remaster effect; proj_b != 0 means a scene projection was captured this frame
     * (menu/HUD frames leave it 0 -> no AO). WebGPU is never MSAA, so the GL/Metal
     * "SSAO off under MSAA" limit does not apply. Video.SsaoMode=hemisphere (v2) is
     * a Metal-only effect; like GL, WebGPU falls back to planar v1 with a one-time
     * note. */
    int ssao_on = (apply_post && g_pcSsao != 0 && g_pc_ssao_proj_b != 0.0f) ? 1 : 0;
    if (ssao_on && g_pcSsaoMode == 2) {
        static int warned_ssao_mode;
        if (!warned_ssao_mode) {
            fprintf(stderr, "[webgpu] Video.SsaoMode=hemisphere is a Metal-only effect; "
                            "WebGPU falls back to planar SSAO v1.\n");
            warned_ssao_mode = 1;
        }
    }
    u.ssao = ssao_on;
    u.linearFinish =
        (apply_post &&
         (u.tonemap != 0 || gp != 0 || u.bloom != 0 ||
          ssao_on != 0 || u.saturation != 1.0f ||
          u.contrast != 1.0f || u.brightness != 0.0f))
            ? 1 : 0;
    u.ssaoRadius = g_pcSsaoRadius * 0.02f;   /* radius key -> UV offset scale (load-bearing) */
    u.ssaoIntensity = g_pcSsaoIntensity;
    u.ssaoAspect = s_scene_h > 0 ? (float)s_scene_w / (float)s_scene_h : 1.0f;
    u.ssaoProjA = g_pc_ssao_proj_a;
    u.ssaoProjB = g_pc_ssao_proj_b;
    wgpuQueueWriteBuffer(s_queue, s_post_ubuf, 0, &u, sizeof(u));

    /* (Re)build the bind group when the scene view changes (resolution change). */
    if (s_post_bg == NULL || s_post_bg_view != s_scene_view) {
        if (s_post_bg != NULL) { wgpuBindGroupRelease(s_post_bg); s_post_bg = NULL; }
        WGPUBindGroupEntry be[6] = {0};
        be[0].binding = 0; be[0].buffer = s_post_ubuf; be[0].size = sizeof(WgpuPostU);
        be[1].binding = 1; be[1].textureView = s_scene_view;
        be[2].binding = 2; be[2].sampler = s_post_sampN;
        be[3].binding = 3; be[3].sampler = s_post_sampL;
        /* SSAO depth: bound unconditionally (the shader only reads it when u.ssao==1,
         * so faithful/SSAO-off frames are unaffected). s_depth_view is recreated in
         * lockstep with s_scene_view, so the same rebuild trigger covers it. The
         * nearest/clamp s_post_sampN doubles as the NonFiltering depth sampler. */
        be[4].binding = 4; be[4].textureView = s_depth_view;
        be[5].binding = 5; be[5].sampler = s_post_sampN;
        WGPUBindGroupDescriptor bgd = {0};
        bgd.layout = s_post_bgl;
        bgd.entryCount = 6;
        bgd.entries = be;
        s_post_bg = WGPU_FAULT_CREATE(
            POST_BIND_GROUP, wgpuDeviceCreateBindGroup(s_device, &bgd));
        if (s_post_bg != NULL) {
            s_post_bg_view = s_scene_view;
        }
    }
    if (s_post_bg == NULL) {
        fprintf(stderr, "[webgpu] post-effect bind-group creation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not bind its post-processing inputs. "
            "Reload the page to continue from the last persisted save.");
        return false;
    }

    WGPURenderPassColorAttachment att = {0};
    att.view = target;
    att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    att.loadOp = WGPULoadOp_Clear;   /* fullscreen triangle covers all pixels */
    att.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp = {0};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;
    WGPURenderPassEncoder pass = WGPU_FAULT_CREATE(
        POST_PASS, wgpuCommandEncoderBeginRenderPass(s_encoder, &rp));
    if (pass == NULL) {
        fprintf(stderr, "[webgpu] post-effect pass creation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not begin its post-processing pass. "
            "Reload the page to continue from the last persisted save.");
        return false;
    }
    wgpuRenderPassEncoderSetPipeline(pass, s_post_pipe);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, s_post_bg, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    return true;
}

/*
 * UI-3: finish the supersampled/post-processed scene, resolve it into the
 * persistent output-sized target, then reopen the normal Fast3D draw path with
 * Load semantics. All subsequent authored SAFE_2D/FULLBLEED geometry therefore
 * lands at one physical pixel per output pixel and is neither downsampled nor
 * passed through world post-processing.
 */
static bool wgpu_begin_output_overlay(void) {
    WGPUTextureView source_view;
    WGPURenderPassColorAttachment color = {0};
    WGPURenderPassDepthStencilAttachment depth = {0};
    WGPURenderPassDescriptor rp = {0};

    if (s_output_overlay_active) {
        return true;
    }
    if (!s_frame_open || s_encoder == NULL || s_pass == NULL ||
        s_scene_view == NULL || s_resolve_view == NULL ||
        s_output_depth_view == NULL ||
        s_scene_w == 0 || s_scene_h == 0 ||
        s_resolve_w == 0 || s_resolve_h == 0) {
        return false;
    }

    wgpuRenderPassEncoderEnd(s_pass);
    wgpuRenderPassEncoderRelease(s_pass);
    s_pass = NULL;
    if (s_runtime_status == GFX_RENDERING_FATAL) {
        return false;
    }

    source_view = s_scene_view;
    if (wgpu_postfx_active()) {
        if (!wgpu_run_postfx(s_post_view)) {
            return false;
        }
        source_view = s_post_view;
    }
    if (!wgpu_run_resolve(s_encoder, source_view, s_scene_w, s_scene_h,
                          s_resolve_w, s_resolve_h)) {
        return false;
    }

    color.view = s_resolve_view;
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    color.loadOp = WGPULoadOp_Load;
    color.storeOp = WGPUStoreOp_Store;
    depth.view = s_output_depth_view;
    depth.depthLoadOp = WGPULoadOp_Clear;
    depth.depthStoreOp = WGPUStoreOp_Store;
    depth.depthClearValue = 1.0f;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &color;
    rp.depthStencilAttachment = &depth;
    s_pass = WGPU_FAULT_CREATE(
        OUTPUT_OVERLAY_PASS,
        wgpuCommandEncoderBeginRenderPass(s_encoder, &rp));
    if (s_pass == NULL) {
        fprintf(stderr, "[webgpu] output-overlay pass creation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not begin the output-resolution HUD "
            "pass. Reload the page to continue from the last persisted save.");
        return false;
    }
    s_output_overlay_active = true;
    s_present_target_tex = s_resolve_tex;
    s_present_target_view = s_resolve_view;
    wgpu_reset_pass_dynamic_state();
    return true;
}

/* PERF-008: could ANY frame in this session be read back or dumped? The offscreen
 * present target must be retained (and the trailing texture-to-texture present copy
 * kept) for such frames, because read_framebuffer_rgb + the frame/surface PPM dumps
 * read s_present_target_tex AFTER present — a surface view is freed by then. Every
 * trigger is armed at process start (a CLI flag or an env var), so a sticky latch is
 * a safe conservative SUPERSET:
 *   - the --screenshot-frame session (g_screenshotFrameSessionActive), its armed frame
 *     (g_autoScreenshotFrame) and --screenshot-game-timer (g_autoScreenshotGameTimer),
 *     which the parity/oracle screenshot ctests drive;
 *   - the AUDIT-0003 screenshot series (GE007_SCREENSHOT_SERIES_DIR);
 *   - the frame-30 GE007_SCREENSHOT one-shot;
 *   - the in-end_frame WebGPU PPM dumps (GE007_WEBGPU_DUMP_FRAME / _SURFACE);
 *   - the diag display-cast / menu captures;
 *   - the F2 manual screenshot, itself inert unless GE007_DEV_HOTKEYS is set;
 *   - the gfx_pc.c diagnostic pixel probes (GE007_TRACE_*_PIXEL / _FB_CAPTURE), which
 *     read back mid-frame; the sticky s_readback_latched (tripped by the first actual
 *     read_framebuffer_rgb) also nets these — and any future reader — from then on.
 * When none are armed (normal gameplay / the web demo) a post-FX frame renders
 * straight into the surface. Sticky so a flag that self-clears after firing (the
 * auto-screenshot frame/timer reset to -1) still pins us to the offscreen path.
 * Correctness-first: any unrecognised readback route ⇒ keep the offscreen path. */
static bool wgpu_readback_possible(void) {
    if (s_readback_latched) {
        return true;
    }
    extern int g_screenshotFrameSessionActive;
    extern int g_autoScreenshotFrame;
    extern int g_autoScreenshotGameTimer;
    extern const char *g_dumpFramesDir;
    if (g_screenshotFrameSessionActive || g_autoScreenshotFrame >= 0 ||
        g_autoScreenshotGameTimer >= 0 || g_dumpFramesDir != NULL) {
        s_readback_latched = true;
        return true;
    }
    static int env_armed = -1;   /* env triggers are process-constant; parse once */
    if (env_armed < 0) {
        env_armed = (getenv("GE007_SCREENSHOT_SERIES_DIR")             != NULL ||
                     getenv("GE007_SCREENSHOT")                        != NULL ||
                     getenv("GE007_WEBGPU_DUMP_FRAME")                 != NULL ||
                     getenv("GE007_WEBGPU_DUMP_SURFACE")               != NULL ||
                     getenv("GE007_DIAG_DISPLAYCAST_SCREENSHOT_TIMER") != NULL ||
                     getenv("GE007_DIAG_MENU_SCREENSHOT_MENU")         != NULL ||
                     getenv("GE007_DEV_HOTKEYS")                       != NULL ||
                     getenv("GE007_TRACE_SETTEX_FB_CAPTURE")           != NULL ||
                     getenv("GE007_TRACE_SETTEX_PIXEL")                != NULL ||
                     getenv("GE007_TRACE_TRI_PIXEL")                   != NULL ||
                     getenv("GE007_TRACE_ROOM_XLU_DEFER_PIXEL")        != NULL ||
                     getenv("GE007_TRACE_SKY_PREP_PIXEL")              != NULL) ? 1 : 0;
    }
    if (env_armed) {
        s_readback_latched = true;
    }
    return env_armed != 0;
}

static void wgpu_end_frame(void) {
    (void)wgpu_consume_callback_failure();
    if (!s_frame_open) {
        /* start_frame bailed (not ready / no texture) — nothing to submit. */
        return;
    }
    /*
     * A mid-frame boundary (notably output-overlay startup) may have already
     * ended the scene pass and then failed to create its replacement. In that
     * state the fatal latch is the transaction result; calling WebGPU with the
     * null replacement would panic in native wgpu before recovery can run.
     */
    if (s_pass != NULL) {
        wgpuRenderPassEncoderEnd(s_pass);
        wgpuRenderPassEncoderRelease(s_pass);
        s_pass = NULL;
    } else if (s_runtime_status != GFX_RENDERING_FATAL) {
        wgpu_runtime_fatal(
            "The graphics backend lost its active render pass. Reload the page "
            "to continue from the last persisted save.");
    }
    const bool output_overlay = s_output_overlay_active;
    /*
     * A draw/resource helper may have latched a fatal error while recording the
     * scene. Do not let a subsequently successful surface acquisition overwrite
     * that state with READY, and do not submit a deliberately incomplete frame.
     * Releasing an unfinished encoder is the transactional rollback for this
     * command stream; the scheduler observes FATAL immediately after return.
     */
    if (s_runtime_status == GFX_RENDERING_FATAL) {
        if (s_encoder != NULL) {
            wgpuCommandEncoderRelease(s_encoder);
            s_encoder = NULL;
        }
        s_frame_open = false;
        return;
    }

    /* PERF-005b: a frame that dropped draw batches because their async pipelines
     * are still compiling (PERF-005 web-live path) is visually incomplete — the
     * missing world geometry reads as sky/backdrop "bleeding" through walls at
     * level entry and on first-sight materials mid-mission. Do NOT present such a
     * frame: skip the surface acquire below entirely, so the canvas (web: the
     * canvas only updates on a frame whose getCurrentTexture was taken) / window
     * (native: present is guarded on present_ok) keeps the LAST COMPLETE image
     * while the offscreen scene still renders, the sim advances, and the end-of-
     * frame drain lands the pending pipelines. Typical hold is 1-4 frames.
     * Bounded: after WGPU_PRESENT_HOLD_MAX consecutive holds we present anyway,
     * so a wedged compile can never freeze the output (and FAILED pipelines never
     * count toward the hold — see s_frame_pending_skips). Native and web
     * --deterministic never store PENDING slots, so the counter is permanently 0
     * there and this block is behavior-neutral for every byte-exact gate. */
    /* The completed real walk is the exact alpha-zero endpoint. Present it
     * normally; only later midpoint replays replace the surface image. Holding
     * it here used to force a delayed alpha-zero redraw after the game had
     * already built its next list, exposing mutable dependencies to replay. */
    bool hold_present = false;
    if (s_frame_pending_skips > 0 &&
        s_present_hold_streak < WGPU_PRESENT_HOLD_MAX) {
        hold_present = true;
        s_present_hold_streak++;
        s_async_present_hold_frames++;
        if (s_present_hold_streak > s_present_hold_streak_max) {
            s_present_hold_streak_max = s_present_hold_streak;
        }
    } else {
        s_present_hold_streak = 0;
    }
    s_frame_pending_skips = 0;
    if (hold_present) {
        s_gpu_surface_holds++;
    }

    /* Acquire the window drawable up front: the PERF-008 direct-to-surface path below
     * renders the output filter straight into it, and the offscreen path uses it as
     * the present-copy destination. A hidden/occluded window has no drawable — that's
     * fine, the offscreen scene still rendered (and can be dumped/read back). Exactly
    * one GetCurrentTexture per frame, as before (just hoisted above the resolve). */
    WGPUSurfaceTexture st = {0};
    /*
     * s_cfg_w/h are the extent of the surface this frame already rendered and
     * is about to present into; the reconfigure they request belongs to the NEXT
     * frame's start_frame. Zeroing them here would hand 0x0 to the present blit
     * (which rejects it) and to the overlay/dump extents, turning a routine
     * suboptimal acquire during a resize into a fatal reload panel.
     */
    bool surface_reconfigure_pending = false;
    if (!hold_present) {
        wgpuSurfaceGetCurrentTexture(s_surface, &st);
        WGPUSurfaceGetCurrentTextureStatus actual_status = st.status;
        WGPUSurfaceGetCurrentTextureStatus injected_status = st.status;
        if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_SUBOPTIMAL)) {
            injected_status =
                WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
        } else if (gfx_webgpu_fault_hit(
                       GFX_WEBGPU_FAULT_SURFACE_TIMEOUT)) {
            injected_status = WGPUSurfaceGetCurrentTextureStatus_Timeout;
        } else if (gfx_webgpu_fault_hit(
                       GFX_WEBGPU_FAULT_SURFACE_OUTDATED)) {
            injected_status = WGPUSurfaceGetCurrentTextureStatus_Outdated;
        } else if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_LOST)) {
            injected_status = WGPUSurfaceGetCurrentTextureStatus_Lost;
        } else if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_ERROR)) {
            injected_status = WGPUSurfaceGetCurrentTextureStatus_Error;
        }
        if (injected_status != st.status) {
            if (injected_status !=
                    WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
                injected_status !=
                    WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal &&
                st.texture != NULL) {
                wgpuTextureRelease(st.texture);
                st.texture = NULL;
            }
            st.status = injected_status;
        }
        /*
         * Some native dialects report generic Error rather than Occluded while
         * an SDL window is hidden/minimized. Only normalize the real driver
         * result; an explicitly injected surface.error must retain fatal
         * semantics for the fault matrix.
         */
        if (injected_status == actual_status &&
            st.status == WGPUSurfaceGetCurrentTextureStatus_Error &&
            !platform_sdl_surface_presentable()) {
            st.status = WGPUSurfaceGetCurrentTextureStatus_Timeout;
        }
    }
    bool present_ok = st.texture != NULL &&
        (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
         st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal);
    if (!hold_present && !present_ok) {
        s_gpu_surface_unavailable++;
    }
    if (getenv("MDKR_TEST_VISIBLE_HEADLESS") != NULL) {
        static bool reported_test_surface;
        if (!reported_test_surface) {
            fprintf(stderr,
                    "[webgpu-test] bounded visible surface status=%d "
                    "texture=%s copyDst=%s copySrc=%s\n",
                    (int)st.status, st.texture != NULL ? "yes" : "no",
                    s_surface_copy_dst ? "yes" : "no",
                    s_surface_copy_src ? "yes" : "no");
            reported_test_surface = true;
        }
    }
    bool surface_retry_failed = false;

    if (!hold_present && WGPU_COMPAT_SURFACE_OCCLUDED(st.status)) {
        /* Native Metal extension: a hidden/headless/covered window has no
         * drawable. The offscreen frame remains valid; retry when visible. */
        s_surface_recovery_attempts = 0;
        s_runtime_status = GFX_RENDERING_TRANSIENT;
    } else if (!hold_present) {
        switch (st.status) {
            case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
                s_surface_recovery_attempts = 0;
                s_runtime_status = GFX_RENDERING_READY;
                break;
            case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
                s_surface_recovery_attempts = 0;
                surface_reconfigure_pending = true;
                s_runtime_status = GFX_RENDERING_TRANSIENT;
                break;
            case WGPUSurfaceGetCurrentTextureStatus_Timeout:
                s_runtime_status = GFX_RENDERING_TRANSIENT;
                surface_retry_failed = true;
                break;
            case WGPUSurfaceGetCurrentTextureStatus_Outdated:
            case WGPUSurfaceGetCurrentTextureStatus_Lost:
                surface_reconfigure_pending = true;
                s_runtime_status = GFX_RENDERING_TRANSIENT;
                surface_retry_failed = true;
                break;
            case WGPUSurfaceGetCurrentTextureStatus_Error:
            default:
                fprintf(stderr,
                        "[webgpu] unrecoverable surface acquisition status=%d\n",
                        (int)st.status);
                s_ready = false;
                s_runtime_status = GFX_RENDERING_FATAL;
                WGPU_COMPAT_REPORT_FAILURE(
                    "The graphics surface failed. Reload the page to continue "
                    "from the last persisted save.");
                break;
        }
    }
    if (surface_retry_failed &&
        ++s_surface_recovery_attempts >= WGPU_SURFACE_RECOVERY_LIMIT) {
        fprintf(stderr,
                "[webgpu] surface recovery failed for %u consecutive frames "
                "(status=%d)\n",
                WGPU_SURFACE_RECOVERY_LIMIT,
                (int)st.status);
        wgpu_runtime_fatal(
            "The graphics surface could not be recovered. Reload the page to "
            "continue from the last persisted save.");
    }

    /* PERF-008: when the output filter is active AND this frame is never read back,
     * render the FINAL post-FX pass straight into the surface, eliminating the
     * trailing full-resolution texture-to-texture present copy (~4-15 MB/frame plus a
     * store on tile-based GPUs). The offscreen present target + copy are retained for
     * any frame that can be read back — the screenshot/parity/dump harness reads
     * s_present_target_tex AFTER present (AUDIT-0003) and a surface view is gone by
     * then — and for faithful/no-post-FX frames (no fullscreen pass to redirect, and a
     * blit would cost as much as the copy). wgpu_readback_possible() is a conservative
     * session-level superset; in any doubt we keep the byte-identical offscreen path. */
    WGPUTextureView surface_view = NULL;
    bool direct = false;
    /* mdkr64: the direct path writes the post-FX pass straight into the surface,
     * which is OUTPUT-sized. With Video.RenderScale > 1 the scene is larger and
     * must go through wgpu_run_resolve() first, so the shortcut is unavailable. */
    bool supersampling = (s_scene_w != s_resolve_w || s_scene_h != s_resolve_h) &&
                         s_resolve_view != NULL;
    /*
     * The output-resolution UI makes the old no-overlay direct route rare.
     * Permit one explicitly requested eligibility attempt so fault injection
     * can prove that a failed surface-view constructor falls back to the
     * complete offscreen frame. One-shot is essential: after the injected
     * failure, later frames must retain their real output-overlay decision.
     */
    static bool test_direct_attempted;
    bool test_force_direct =
        !test_direct_attempted &&
        getenv("MDKR_TEST_WEBGPU_DIRECT_VIEW") != NULL;
    if (test_force_direct && !present_ok &&
        getenv("MDKR_TEST_OCCLUDED_SURFACE_FAULTS") != NULL &&
        gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_DIRECT_VIEW)) {
        /*
         * The offscreen frame is already complete. When the compositor has
         * withheld the drawable, consume the requested direct-view failure at
         * this eligibility boundary and retain that complete offscreen frame,
         * exactly as a NULL surface view would.
         */
        fprintf(stderr,
                "[webgpu-test] no surface drawable; injecting configured "
                "direct-view failure at the present boundary\n");
        test_direct_attempted = true;
    }
    if ((!output_overlay || test_force_direct) &&
        present_ok && !supersampling &&
        !wgpu_readback_possible() && wgpu_postfx_active()) {
        if (test_force_direct) {
            test_direct_attempted = true;
        }
        surface_view = WGPU_FAULT_CREATE(
            SURFACE_DIRECT_VIEW, wgpuTextureCreateView(st.texture, NULL));
        if (surface_view != NULL && wgpu_run_postfx(surface_view)) {
            direct = true;
            /* Presented pixels now live only in the surface (released after present),
             * so there is no persistent readback target — safe, since the gate above
             * proved this frame is never read back. NULL steers any stray readback to
             * the still-valid raw scene rather than a freed surface texture. */
            s_present_target_tex  = NULL;
            s_present_target_view = surface_view;
        } else if (surface_view != NULL) {
            wgpuTextureViewRelease(surface_view);   /* post-FX unavailable; fall back */
            surface_view = NULL;
        }
    }

    /* Offscreen present path (byte-identical to the pre-PERF-008 backend). Resolve the
     * raw scene through the VI filter into s_post_tex when active — THAT becomes the
     * frame that is minimap-composited, copied to the surface and read back — matching
     * GL (output filter into the default FB, THEN the minimap on top) and Metal
     * (s_final_color, THEN minimap). A faithful frame (filter inactive) keeps the raw
     * scene as the present source. The minimap is drawn AFTER the filter so it is not
     * tonemapped/graded, exactly as on GL/Metal. */
    if (output_overlay) {
        s_present_target_tex = s_resolve_tex;
        s_present_target_view = s_resolve_view;
    } else if (!direct) {
        s_present_target_tex  = s_scene_tex;
        s_present_target_view = s_scene_view;
        if (wgpu_postfx_active() && wgpu_run_postfx(s_post_view)) {
            s_present_target_tex  = s_post_tex;
            s_present_target_view = s_post_view;
        }
    }

    /*
     * Supersample resolve. The present copy below is a texture-to-texture copy
     * and requires matching extents, so this is what lets a scaled scene reach
     * an output-sized surface at all. Resolving into a persistent texture (not
     * the surface) is what keeps readback and the frame dumper output-sized.
     */
    bool resolved_frame = output_overlay || !supersampling;
    if (!output_overlay && !direct && supersampling &&
        s_present_target_view != NULL) {
        if (wgpu_run_resolve(s_encoder, s_present_target_view, s_scene_w, s_scene_h,
                             s_resolve_w, s_resolve_h)) {
            s_present_target_tex  = s_resolve_tex;
            s_present_target_view = s_resolve_view;
            resolved_frame = true;
        } else {
            /* Never present a scaled scene through a size-matched copy: it would
             * fail validation or crop. Keep the prior complete surface frame;
             * start_frame will retry the resolve on the next frame. */
            static bool warned_resolve = false;
            if (!warned_resolve) {
                fprintf(stderr, "[webgpu] supersample resolve unavailable; "
                                "holding the previous surface frame\n");
                warned_resolve = true;
            }
            present_ok = false;
        }
    }

    /* Minimap / radar overlay: a 2D screen-space pass into the present target after
     * the post-FX (the GL path draws it in gfx_end_frame, Metal in mtl_end_frame;
     * gfx_end_frame skips it for non-GL backends). Reads Input.MinimapEnabled +
     * the frame queue internally; no-op when disabled/empty. */
    if (resolved_frame) {
        extern void minimap_overlay_draw_queued_frames_webgpu(int fb_width, int fb_height);
        const int target_w = (output_overlay || supersampling)
            ? (int)s_resolve_w : (int)s_scene_w;
        const int target_h = (output_overlay || supersampling)
            ? (int)s_resolve_h : (int)s_scene_h;
        minimap_overlay_draw_queued_frames_webgpu(target_w, target_h);
    }

    /* Optional debug frame dump: copy the presented frame (post-FX + minimap) into
     * a mappable buffer (works even when the window is hidden, unlike a surface
     * readback). */
    static int frame_no = -1;
    frame_no++;
    WGPUBuffer dump_buf = NULL;
    uint32_t dump_bpr = 0;
    /* Dimensions follow the PRESENT TARGET, which is the output-sized resolve
     * texture on a supersampled frame and the scene otherwise. */
    uint32_t dump_w = (output_overlay || supersampling)
        ? s_resolve_w : s_scene_w;
    uint32_t dump_h = (output_overlay || supersampling)
        ? s_resolve_h : s_scene_h;
    if (resolved_frame && frame_no == wgpu_dump_target_frame() &&
        dump_w > 0 && dump_h > 0) {
        dump_bpr = ((dump_w * 4u + 255u) / 256u) * 256u;   /* 256-align for CopyTextureToBuffer */
        WGPUBufferDescriptor bd = {0};
        bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        bd.size = (uint64_t)dump_bpr * dump_h;
        dump_buf = WGPU_FAULT_CREATE(
            FRAME_DUMP_BUFFER, wgpuDeviceCreateBuffer(s_device, &bd));
        if (dump_buf != NULL) {
            WGPUTexelCopyTextureInfo src = {0};
            src.texture = s_present_target_tex; src.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyBufferInfo dst = {0};
            dst.buffer = dump_buf;
            dst.layout.bytesPerRow = dump_bpr;
            dst.layout.rowsPerImage = dump_h;
            WGPUExtent3D ext = { dump_w, dump_h, 1 };
            wgpuCommandEncoderCopyTextureToBuffer(s_encoder, &src, &dst, &ext);
        }
    }

    /* Present copy: OFFSCREEN path only. The direct path (PERF-008) already rendered
     * the final frame straight into the surface, so it needs no copy. Same format +
     * output size (the supersample resolve supplies that size when active). A
     * hidden/occluded window has no drawable — present is skipped; the offscreen
     * frame still rendered and read back. */
    if (present_ok && !direct && resolved_frame) {
        const uint32_t present_w = (output_overlay || supersampling)
            ? s_resolve_w : s_scene_w;
        const uint32_t present_h = (output_overlay || supersampling)
            ? s_resolve_h : s_scene_h;
        if (s_surface_copy_dst) {
            WGPUTexelCopyTextureInfo cs = {0};
            WGPUTexelCopyTextureInfo cd = {0};
            WGPUExtent3D ext = { present_w, present_h, 1 };
            cs.texture = s_present_target_tex;
            cs.aspect = WGPUTextureAspect_All;
            cd.texture = st.texture;
            cd.aspect = WGPUTextureAspect_All;
            wgpuCommandEncoderCopyTextureToTexture(
                s_encoder, &cs, &cd, &ext);
        } else {
            surface_view = WGPU_FAULT_CREATE(
                SURFACE_BLIT_VIEW, wgpuTextureCreateView(st.texture, NULL));
            if (surface_view == NULL ||
                !wgpu_run_resolve_to(
                    s_encoder, s_present_target_view, present_w, present_h,
                    surface_view, s_cfg_w, s_cfg_h)) {
                fprintf(stderr,
                        "[webgpu] render-pass surface blit failed\n");
                if (surface_view != NULL) {
                    wgpuTextureViewRelease(surface_view);
                    surface_view = NULL;
                }
                s_ready = false;
                s_runtime_status = GFX_RENDERING_FATAL;
                WGPU_COMPAT_REPORT_FAILURE(
                    "The graphics backend could not present a frame. Reload "
                    "the page to continue from the last persisted save.");
            }
        }
    } else if (!present_ok && !direct && resolved_frame &&
               !s_surface_copy_dst &&
               getenv("MDKR_TEST_OCCLUDED_SURFACE_FAULTS") != NULL &&
               gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_BLIT_VIEW)) {
        /*
         * A newly shown native Metal window is not guaranteed to receive a
         * drawable before a bounded headless integration run ends. Consume
         * the requested create-view fault at the same present boundary so the
         * recovery test cannot pass or fail according to compositor timing.
         * This branch is unreachable without the explicit test-only seam.
         */
        fprintf(stderr,
                "[webgpu-test] no surface drawable; injecting configured "
                "blit-view failure at the present boundary\n");
        fprintf(stderr, "[webgpu] render-pass surface blit failed\n");
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend could not present a frame. Reload the page "
            "to continue from the last persisted save.");
    }

    /* In-game overlay (F1 menu). The hook is called EXACTLY ONCE every frame —
     * matching the GL gfx_end_frame's unconditional call — so its per-frame logic
     * (e.g. the headless open/close test tick, which advances the engine-frame
     * ordinal) stays in lockstep even on a frame that drops its drawable.
     * The bandwidth-heavy Load+Store surface pass only opens when we actually
     * have a drawable AND the menu or FPS readout is visible
     * (platformOverlayWantsRender); input capture remains a separate menu-only
     * decision in platformOverlayWantsInput. Otherwise the overlay's draw is
     * skipped (current_overlay_pass() == NULL), so
     * standalone boots (no hooks → 0) and closed-overlay gameplay pay nothing. */
    extern int  platformOverlayRender(void);
    extern int  platformOverlayWantsRender(void);
    int overlay_render_ok = 1;
    bool overlay_target_failed = false;
    const bool overlay_requested =
        present_ok && platformOverlayWantsRender();
    WGPUTextureView overlay_view = NULL;
    if (overlay_requested) {
        overlay_view = WGPU_FAULT_CREATE(
            OVERLAY_VIEW, wgpuTextureCreateView(st.texture, NULL));
#ifdef MDKR_APP
        overlay_target_failed = overlay_view == NULL;
#endif
    }
    if (overlay_view != NULL) {
        WGPURenderPassColorAttachment oa = {0};
        oa.view = overlay_view;
        oa.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        oa.loadOp = WGPULoadOp_Load;      /* preserve the blitted scene */
        oa.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor orp = {0};
        orp.colorAttachmentCount = 1;
        orp.colorAttachments = &oa;
        s_overlay_pass = WGPU_FAULT_CREATE(
            OVERLAY_PASS,
            wgpuCommandEncoderBeginRenderPass(s_encoder, &orp));
        if (s_overlay_pass == NULL) {
            wgpuTextureViewRelease(overlay_view);
            overlay_view = NULL;
            if (!s_overlay_failure_reported) {
#ifdef MDKR_APP
                fprintf(stderr,
                        "[webgpu] required app overlay pass unavailable\n");
#else
                fprintf(stderr,
                        "[webgpu] overlay pass unavailable; presenting game "
                        "frame without overlay\n");
#endif
                s_overlay_failure_reported = true;
            }
#ifdef MDKR_APP
            overlay_target_failed = true;
#endif
            /* Keep the per-frame UI state machine in lockstep even without a
             * pass. Browser/CLI overlays are optional local degradation; the
             * native app marks a requested missing pass fatal below. */
            overlay_render_ok = platformOverlayRender();
        }
        /* The overlay pass targets the swapchain, which always stays in OUTPUT
         * space even when the scene is supersampled. */
        s_overlay_w = (int)s_cfg_w;
        s_overlay_h = (int)s_cfg_h;
        if (s_overlay_pass != NULL) {
            overlay_render_ok = platformOverlayRender();
            wgpuRenderPassEncoderEnd(s_overlay_pass);
            wgpuRenderPassEncoderRelease(s_overlay_pass);
            s_overlay_pass = NULL;
            wgpuTextureViewRelease(overlay_view);
        }
    } else {
        overlay_render_ok = platformOverlayRender(); /* state only; no pass/draw */
    }
    if (overlay_target_failed || !overlay_render_ok) {
        fprintf(stderr, "[webgpu] ImGui overlay rendering failed\n");
        /* Do not expose a frame whose required menu was silently omitted. The
         * command encoder is still finished/submitted below so all already-
         * encoded child resources retire normally, but this drawable is not
         * presented and the engine observes a typed fatal renderer state. */
        present_ok = false;
        wgpu_runtime_fatal(
            "The in-game graphics menu could not be rendered. Restart the app "
            "to continue from the last persisted save.");
    }

    /* Optional surface dump: the presented frame (scene + overlay), unlike the
     * scene dump above which is overlay-free. Requires the surface CopySrc usage. */
    WGPUBuffer surf_dump_buf = NULL;
    uint32_t surf_dump_bpr = 0;
    /*
     * A native headless/occluded surface may never vend a drawable, which used
     * to make the surface-buffer failure arm nondeterministic. When that exact
     * diagnostic fault is selected, consume its visit at the requested frame
     * even without a drawable. The injected constructor result is NULL, so this
     * is behavior-identical to the allocation-failure branch and never issues a
     * copy against st.texture. Ordinary execution and every other fault retain
     * the real present_ok prerequisite.
     */
    if (!present_ok &&
        frame_no == wgpu_dump_surface_frame() &&
        gfx_webgpu_fault_selected(GFX_WEBGPU_FAULT_SURFACE_DUMP_BUFFER)) {
        (void)gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_DUMP_BUFFER);
    }
    if (present_ok &&
        frame_no == wgpu_dump_surface_frame() &&
        s_surface_copy_src &&
        s_cfg_w > 0 && s_cfg_h > 0) {
        surf_dump_bpr = ((s_cfg_w * 4u + 255u) / 256u) * 256u;
        WGPUBufferDescriptor bd = {0};
        bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        bd.size = (uint64_t)surf_dump_bpr * s_cfg_h;
        surf_dump_buf = WGPU_FAULT_CREATE(
            SURFACE_DUMP_BUFFER, wgpuDeviceCreateBuffer(s_device, &bd));
        if (surf_dump_buf != NULL) {
            WGPUTexelCopyTextureInfo src = {0};
            src.texture = st.texture; src.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyBufferInfo dst = {0};
            dst.buffer = surf_dump_buf;
            dst.layout.bytesPerRow = surf_dump_bpr;
            dst.layout.rowsPerImage = s_cfg_h;
            WGPUExtent3D ext = { s_cfg_w, s_cfg_h, 1 };
            wgpuCommandEncoderCopyTextureToBuffer(s_encoder, &src, &dst, &ext);
        }
    }

    /* WEB-023 (residual): one vertex upload per active segment. Every scene draw and the
     * minimap overlay memcpy'd their batch into s_vbuf_shadow at its bump offset;
     * push the current segment's referenced range [0, s_vbuf_off) to the GPU in
     * one wgpuQueueWriteBuffer here. A full earlier segment was already flushed
     * when it rotated. Normal content stays within one segment, replacing the
     * ~100-200 per-batch writes (each a wasm↔JS crossing + a queue command).
     * Queue writes execute before this command
     * buffer (WEB-053), so every draw's SetVertexBuffer(voff, bytes) reads exactly
     * the bytes staged for it, byte-identical to the old per-batch scheme. Covers the
     * hidden-drawable frame too: no present, but the offscreen scene still drew and
     * is submitted right below. s_vbuf_off is kept 4-byte aligned by the bump, so the
     * size is a valid queue-write size; skipped when the shadow is absent (writers
     * fell back to per-batch writes) or the frame drew no geometry (s_vbuf_off == 0). */
    if (s_vbuf != NULL && s_vbuf_shadow != NULL && s_vbuf_off > 0) {
        wgpuQueueWriteBuffer(s_queue, s_vbuf, 0, s_vbuf_shadow, s_vbuf_off);
    }

    WGPUCommandBuffer cmd = WGPU_FAULT_CREATE(
        FRAME_FINISH, wgpuCommandEncoderFinish(s_encoder, NULL));
    if (cmd == NULL) {
        fprintf(stderr, "[webgpu] command buffer creation failed\n");
        wgpuCommandEncoderRelease(s_encoder);
        s_encoder = NULL;
        if (dump_buf != NULL) {
            wgpuBufferRelease(dump_buf);
        }
        if (surf_dump_buf != NULL) {
            wgpuBufferRelease(surf_dump_buf);
        }
        if (surface_view != NULL) {
            if (s_present_target_view == surface_view) {
                s_present_target_view = NULL;
            }
            wgpuTextureViewRelease(surface_view);
        }
        s_frame_open = false;
        if (st.texture != NULL) {
            wgpuTextureRelease(st.texture);
        }
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend could not submit a frame. Reload the page "
            "to continue from the last persisted save.");
        return;
    }
    bool submitted = wgpu_submit_commands(s_queue, 1, &cmd);
    if (submitted) {
        wgpu_track_frame_submission();
    }
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(s_encoder);
    s_encoder = NULL;

    /* Dump targets go through savedirPath(), the same writable-location helper
     * the pipeline-prewarm cache uses. A literal "/tmp/..." is not a path on
     * Windows, which this backend also builds for. */
    if (dump_buf != NULL) {
        if (submitted) {
            char name[64];
            char path[512];
            snprintf(name, sizeof(name), "webgpu_frame_%d.ppm", frame_no);
            snprintf(path, sizeof(path), "%s", savedirPath(name));
            wgpu_write_ppm(dump_buf, dump_bpr, dump_w, dump_h, path);
        }
        wgpuBufferRelease(dump_buf);
    }
    if (surf_dump_buf != NULL) {
        if (submitted) {
            char name[64];
            char path[512];
            snprintf(name, sizeof(name), "webgpu_surface_%d.ppm", frame_no);
            snprintf(path, sizeof(path), "%s", savedirPath(name));
            wgpu_write_ppm(
                surf_dump_buf, surf_dump_bpr, s_cfg_w, s_cfg_h, path);
        }
        wgpuBufferRelease(surf_dump_buf);
    }

    if (present_ok && submitted) {
        /* Dialect seam (gfx_webgpu_compat.h): native presents explicitly via
         * wgpuSurfacePresent; the browser canvas auto-presents when the frame's
         * JS task yields (requestAnimationFrame), and emscripten's binding
         * aborts on an explicit present — so the browser side is a no-op. */
        WGPU_COMPAT_PRESENT(s_surface);
        s_gpu_surface_presents++;
        g_surfaceFrameCounter++;
        /* Queue-depth latency proxy: the admission counter this backend already
         * maintains, read at the present boundary. Frames still in flight here
         * are frames queued ahead of this one, and under FIFO each of them
         * costs a refresh period before this image can reach the glass. A read,
         * not a control input -- backpressure admission is decided by
         * wgpu_backpressure_check_below() and is untouched by this. */
        present_perf_note_queue_depth(s_gpu_frames_in_flight);
    }
    if (surface_view != NULL) {
        wgpuTextureViewRelease(surface_view);   /* PERF-008 direct-path render target */
        /* On a direct frame s_present_target_view aliases this view. It MUST be
         * nulled here so no released view can be dereferenced across the frame
         * boundary. Readback goes through s_present_target_tex today, so nothing
         * currently reads it; the invariant is what keeps future overlay and
         * readback work off a dangling handle. */
        if (s_present_target_view == surface_view) {
            s_present_target_view = NULL;
        }
    }
    if (st.texture != NULL) {
        wgpuTextureRelease(st.texture);
    }

    /* PERF-005: only this renderer-owned drain may dispatch asynchronous
     * pipeline completion. It is skipped in steady state, so no unrelated
     * futures are processed merely to draw an otherwise-ready frame. */
    if (s_pending_pipelines > 0) {
        wgpu_pipeline_callback_owner_drain();
    }

    /* Frame is presented and every consumer of the committed extent has run:
     * NOW invalidate it so start_frame reconfigures the surface immediately. */
    if (surface_reconfigure_pending) {
        s_cfg_w = 0;
        s_cfg_h = 0;
    }

    s_frame_open = false;
}

static void wgpu_finish_render(void) {
    /* No explicit GPU drain is needed here: read_framebuffer_rgb owns its own
     * map/wait handshake, and present already ordered the frame's work. */
}

static enum GfxRenderingStatus wgpu_get_status(void) {
    (void)wgpu_consume_callback_failure();
    return s_runtime_status;
}

/* ------------------------------------------------------------------------
 * Vtable: shaders + render pipelines
 *
 * gfx_pc.c owns the shader-pointer lifecycle: it calls lookup_shader, and on a
 * miss create_and_load_new_shader, then load_shader + shader_get_info + draw.
 * Each ShaderProgram holds the compiled WGSL module, the derived vertex layout,
 * the bind-group + pipeline layouts, and a small cache of WGPURenderPipelines
 * keyed by dynamic state (WebGPU bakes blend/depth/format into the pipeline, so
 * the GL immediate setters collapse into this lazy lookup — same shape as the
 * Metal backend's mtl_pso_for).
 * ---------------------------------------------------------------------- */
/* PERF-005: on web-live the pipeline is created asynchronously (see
 * wgpu_pipeline_for), so a cache slot carries a lifecycle state. EMPTY is 0 so
 * the zero-initialized s_shaders table starts every slot EMPTY. Native (and web
 * under --deterministic) only ever stores READY — the sync path never produces a
 * PENDING/FAILED entry, so its lookup behavior is identical to before PERF-005. */
enum WgpuPipeState {
    WGPU_PIPE_EMPTY = 0,   /* unused slot                                   */
    WGPU_PIPE_PENDING,     /* async create in flight (web-live only)        */
    WGPU_PIPE_READY,       /* pipe is valid and serve-able                  */
    WGPU_PIPE_FAILED       /* async create failed; keep skipping (no re-kick) */
};
struct WgpuPipeEntry { uint32_t key; WGPURenderPipeline pipe; uint8_t state; };

struct ShaderProgram {
    uint64_t shader_id0;
    uint32_t shader_id1;
    struct WgpuShaderInfo info;
    WGPUShaderModule     module;
    WGPUVertexAttribute  vattrs[MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY];
    WGPUBindGroupLayout  bgl;       /* NULL when the combiner samples no textures */
    WGPUPipelineLayout   playout;
    /* Pipeline cache keyed by dynamic (blend|depth) state. Sized well above the
     * realistic combo count per combiner (a handful); the round-robin eviction
     * in wgpu_pipeline_for is a never-leak backstop for the theoretical maximum. */
    struct WgpuPipeEntry pipes[32];
    int npipes;
    int pipe_evict;   /* round-robin slot for the (never-hit) overflow case */
};
#define WGPU_PIPE_CACHE 32

/*
 * ShaderProgram pointers escape this backend: gfx_pc_dkr.c caches them in its
 * ColorCombiner entries. Therefore a live shader may never be moved or reused
 * for another id. The previous fixed array evicted in place after 1,024
 * combiners, turning every frontend pointer to the victim into a pointer to an
 * unrelated shader. Allocate each program separately and grow only the pointer
 * index, which keeps every published address stable.
 *
 * The hard limit is an explicit corruption/runaway guard, not an eviction
 * policy. Reaching it is fatal because silently reusing or dropping a shader
 * produces incorrect geometry. Normal DKR content remains far below it; the
 * high-water census reports the observed count separately.
 */
#define WGPU_SHADER_HARD_LIMIT 4096u
static struct ShaderProgram **s_shaders = NULL;
static size_t s_shader_count = 0;
static size_t s_shader_capacity = 0;
static size_t s_shader_high_water = 0;
static size_t s_pipeline_slots_live = 0;
static size_t s_pipeline_slots_high_water = 0;
static int s_max_attrs_seen = 0;
static int s_max_varyings_seen = 0;
static unsigned s_shader_guard_hits = 0;
static unsigned s_pipeline_failures = 0;
static bool s_limits_reported = false;

/*
 * Synthetic positive control for the no-reuse guard. Production always gets
 * WGPU_SHADER_HARD_LIMIT; the test-only environment value can lower (never
 * raise) it so a normal boot proves that capacity exhaustion latches a fatal
 * renderer state instead of overwriting a ShaderProgram whose pointer escaped
 * into the frontend combiner cache.
 */
static size_t wgpu_shader_effective_limit(void) {
    static size_t effective_limit = 0;
    if (effective_limit == 0) {
        const char *value = getenv("MDKR_TEST_WEBGPU_SHADER_LIMIT");
        effective_limit = WGPU_SHADER_HARD_LIMIT;
        if (value != NULL && value[0] != '\0') {
            char *end = NULL;
            unsigned long parsed = strtoul(value, &end, 10);
            if (end != value && end != NULL && end[0] == '\0' &&
                parsed >= 1 && parsed <= WGPU_SHADER_HARD_LIMIT) {
                effective_limit = (size_t)parsed;
            } else {
                fprintf(stderr,
                        "[webgpu] ignoring invalid test shader limit '%s' "
                        "(expected 1..%u)\n",
                        value, WGPU_SHADER_HARD_LIMIT);
            }
        }
    }
    return effective_limit;
}

/* Currently loaded shader + dynamic blend state (set by load_shader /
 * set_blend_mode, read by draw_triangles). */
static struct ShaderProgram *s_cur_shader = NULL;
static enum GfxBlendMode      s_cur_blend = GFX_BLEND_DISABLED;

/* Persistent draw bind-group cache (see wgpu_draw_triangles). Keyed on the
 * {bgl, view0, samp0, view1, samp1, snapview, diag_ubo,
 * shadow_view, shadow_sampler, shadow_ubo, noise_ubo} pointer tuple. The
 * previous single-entry cache thrashed on every material change — a frame that
 * cycles N materials created N bind groups per frame, and the resulting JS-side
 * object churn (Dawn createBindGroup + object-table inserts, visible in browser
 * CPU profiles) fed GC pauses that degraded 1% lows. Entries persist across
 * frames (materials recur every frame) with round-robin eviction as the
 * never-leak backstop.
 *
 * WEB-068: the key is a RAW POINTER tuple, so a cached entry MUST be invalidated
 * when a texture view it references is released — see wgpu_bg_cache_invalidate_view
 * below. The original design note here claimed "stale views can never be looked up:
 * a deleted texture's view pointer is replaced by the white fallback in the key,
 * which misses." That reasoning only covers the CURRENT draw's freshly-built key; it
 * missed that STALE entries built by earlier draws outlive the texture and keep its
 * raw view pointer. wgpuTextureViewRelease frees the C-handle ADDRESS for reuse while
 * the cached bind group keeps the underlying object alive (a strong ref) — so a later
 * texture whose new view lands at the recycled address FALSE-MATCHED a stale entry and
 * the draw sampled the OLD texture (proven natively: menu glyphs rendered as other
 * glyphs, "Select"→"Гissi2A"). A classic ABA on the handle address, which is the key.
 *
 * PERF-019: that invalidation was a full-table sweep per released view—and a
 * level transition (gfx_clear_texture_cache) deletes every pooled texture, so the storm
 * was texture_count × 512 scans. Each WgpuTexEntry now carries a reverse index of the
 * cache slots referencing its view (wgpu_tex_bg_ref_add on insert), so a release walks
 * only its own slots (wgpu_bg_cache_invalidate_view_indexed). No dedicated level-flush
 * hook was added: the natural flush IS gfx_clear_texture_cache's per-texture delete
 * loop (gfx_pc.c)—which the reverse index already makes cheap—and a distinct backend
 * flush entry point would require touching gfx_pc.c / gfx_rendering_api.h, out
 * of scope here. The browser campaign route reached 501/512 entries, so the
 * table now has 2,048 total ways: enough for >4x measured headroom without
 * changing the key, eviction, or invalidation policy. */
#define WGPU_BG_CACHE 2048           /* total entries; power of two */
#define WGPU_BG_WAYS  4
/* E28 added noise_ubo as the tenth-indexed member: the noise uniform used to be
 * one global buffer (no discriminating state), and is now a per-viewport ring
 * slot, so two draws of the same material in two viewports need distinct
 * entries. */
#define WGPU_BG_KEY_COUNT 11
struct WgpuBgEntry {
    const void *key[WGPU_BG_KEY_COUNT];
    WGPUBindGroup bg;
};
static struct WgpuBgEntry s_bg_cache_tab[WGPU_BG_CACHE];
static uint32_t s_bg_cache_way = 0;  /* round-robin victim way on set overflow */
static uint32_t s_bg_cache_live = 0;
static uint32_t s_bg_cache_high_water = 0;

/* WEB-068: drop every cached draw bind group that references `view` before its
 * C-handle is released, so a future view reusing the freed address cannot false-match
 * a stale entry. The view can occupy key slots 1 (view0), 3 (view1), 5 (snapshot
 * "memory color"), and 7 (world-shadow array); the sampler slots (2, 4, 8), the
 * bgl (0), and the UBO slots (6, 9) are invalidated by their respective lifecycle
 * resets rather than the texture-view reverse index. This full-table sweep is used
 * for the SNAPSHOT and world-shadow views (neither is a WgpuTexEntry) on recreation,
 * and as the PERF-019 overflow fallback — common texture-view releases (delete /
 * recreate-on-resize) go through wgpu_bg_cache_invalidate_view_indexed, which walks
 * a per-texture reverse index instead. The per-slot drop predicate here is the
 * canonical one the indexed path re-verifies against, so the two stay identical. */
static void wgpu_bg_cache_invalidate_view(WGPUTextureView view) {
    if (view == NULL) {
        return;
    }
    const void *v = (const void *)view;
    for (int i = 0; i < WGPU_BG_CACHE; i++) {
        struct WgpuBgEntry *e = &s_bg_cache_tab[i];
        if (e->bg != NULL &&
            (e->key[1] == v || e->key[3] == v ||
             e->key[5] == v || e->key[7] == v)) {
            wgpu_release_cached_bind_group(e->bg);
            e->bg = NULL;
            memset(e->key, 0, sizeof(e->key));
            if (s_bg_cache_live > 0) {
                s_bg_cache_live--;
            }
        }
    }
}
/* Entries pin their WGPUBindGroup (and transitively their views/samplers).
 * Native device recovery releases and clears every live entry before replacing
 * the device; ordinary texture teardown uses the reverse index below. */

static uint32_t wgpu_bg_key_hash(
    const void *const key[WGPU_BG_KEY_COUNT]) {
    uintptr_t h = 0x9e3779b9u;
    for (int i = 0; i < WGPU_BG_KEY_COUNT; i++) {
        h ^= (uintptr_t)key[i] >> 4;   /* pointers are >=16-aligned; drop zeros */
        h *= 0x85ebca6bu;
    }
    return (uint32_t)(h ^ (h >> 16));
}

static uint8_t *s_shadow_upload = NULL;
static size_t s_shadow_upload_cap = 0;

static void wgpu_shadow_release_texture_set(void) {
    if (s_shadow_array_view != NULL) {
        wgpu_bg_cache_invalidate_view(s_shadow_array_view);
    }
    for (size_t index = 0; index < GFX_SHADOW_MAX_MAPS; index++) {
        if (s_shadow_layer_view[index] != NULL) {
            wgpuTextureViewRelease(s_shadow_layer_view[index]);
            s_shadow_layer_view[index] = NULL;
        }
    }
    if (s_shadow_array_view != NULL) {
        wgpuTextureViewRelease(s_shadow_array_view);
        s_shadow_array_view = NULL;
    }
    if (s_shadow_tex != NULL) {
        wgpuTextureRelease(s_shadow_tex);
        s_shadow_tex = NULL;
    }
    if (s_shadow_sampler != NULL) {
        wgpuSamplerRelease(s_shadow_sampler);
        s_shadow_sampler = NULL;
    }
    s_shadow_res = 0;
    s_shadow_layers = 0;
}

static void wgpu_release_shadow_resources(void) {
    wgpu_shadow_release_texture_set();
    for (size_t index = 0; index < GFX_SHADOW_MAX_MAPS; index++) {
        if (s_shadow_matrix_bg[index] != NULL) {
            wgpuBindGroupRelease(s_shadow_matrix_bg[index]);
            s_shadow_matrix_bg[index] = NULL;
        }
        if (s_shadow_matrix_ubo[index] != NULL) {
            wgpuBufferRelease(s_shadow_matrix_ubo[index]);
            s_shadow_matrix_ubo[index] = NULL;
        }
    }
    for (size_t index = 0; index < GFX_SHADOW_MAX_VIEWS; index++) {
        if (s_shadow_receiver_ubo[index] != NULL) {
            wgpuBufferRelease(s_shadow_receiver_ubo[index]);
            s_shadow_receiver_ubo[index] = NULL;
        }
    }
    for (size_t index = 0; index < 2; index++) {
        if (s_shadow_pipeline[index] != NULL) {
            wgpuRenderPipelineRelease(s_shadow_pipeline[index]);
            s_shadow_pipeline[index] = NULL;
        }
    }
    if (s_shadow_pass_layout != NULL) {
        wgpuPipelineLayoutRelease(s_shadow_pass_layout);
        s_shadow_pass_layout = NULL;
    }
    if (s_shadow_pass_bgl != NULL) {
        wgpuBindGroupLayoutRelease(s_shadow_pass_bgl);
        s_shadow_pass_bgl = NULL;
    }
    if (s_shadow_module != NULL) {
        wgpuShaderModuleRelease(s_shadow_module);
        s_shadow_module = NULL;
    }
    if (s_shadow_vbuf != NULL) {
        wgpuBufferRelease(s_shadow_vbuf);
        s_shadow_vbuf = NULL;
    }
    s_shadow_vbuf_cap = 0;
    s_shadow_resource_fail_count = 0;
    s_shadow_resource_fail_res = 0;
    s_shadow_resource_fail_layers = 0;
    s_shadow_resource_perma_fail = false;
#ifdef __EMSCRIPTEN__
    s_shadow_receiver_prewarm_pending = 0;
    s_shadow_receiver_prewarm_failed = false;
    s_shadow_prewarm_memo_valid = false;
#endif
    memset(&s_shadow_plan, 0, sizeof(s_shadow_plan));
    s_shadow_receiver_view = -1;
}

static bool wgpu_shadow_resource_fail(
    uint32_t resolution,
    uint32_t layers,
    const char *reason) {
    s_shadow_resource_fail_count++;
    s_shadow_resource_failures++;
    s_shadow_resource_fail_res = resolution;
    s_shadow_resource_fail_layers = layers;
    if (!s_shadow_resource_perma_fail &&
        s_shadow_resource_fail_count >=
            WGPU_SHADOW_RESOURCE_MAX_FAILURES) {
        s_shadow_resource_perma_fail = true;
        fprintf(stderr,
                "[world-shadow] WebGPU disabled the optional shadow map after "
                "%u consecutive resource failures at res=%u layers=%u "
                "(last=%s); projected shadows remain active\n",
                s_shadow_resource_fail_count,
                resolution, layers, reason);
    }
    return false;
}

static bool wgpu_shadow_resource_retry_allowed(
    uint32_t resolution,
    uint32_t layers) {
#ifdef __EMSCRIPTEN__
    /*
     * A failed receiver pipeline remains a FAILED cache entry until backend
     * recreation. Do not clear the generic latch on a size change and imply
     * that map reallocation can repair shader compilation.
     */
    if (s_shadow_receiver_prewarm_failed) {
        return false;
    }
#endif
    if (s_shadow_resource_perma_fail &&
        (resolution != s_shadow_resource_fail_res ||
         layers != s_shadow_resource_fail_layers)) {
        s_shadow_resource_fail_count = 0;
        s_shadow_resource_perma_fail = false;
    }
    return !s_shadow_resource_perma_fail;
}

static void wgpu_shadow_pack_matrix(
    float output[16],
    const float matrix[4][4]) {
    for (size_t column = 0; column < 4; column++) {
        for (size_t row = 0; row < 4; row++) {
            output[column * 4 + row] = matrix[row][column];
        }
    }
}

static bool wgpu_shadow_ensure_pipeline_resources(void) {
    if (s_shadow_module != NULL &&
        s_shadow_pass_bgl != NULL &&
        s_shadow_pass_layout != NULL &&
        s_shadow_pipeline[0] != NULL &&
        s_shadow_pipeline[1] != NULL) {
        return true;
    }

    static const char source[] =
        "struct ShadowPass { worldToClip : mat4x4<f32> };\n"
        "@group(0) @binding(0) var<uniform> u : ShadowPass;\n"
        "struct ShadowIn { @location(0) position : vec3<f32> };\n"
        "@vertex fn vs_main(input : ShadowIn) -> @builtin(position) vec4<f32> {\n"
        "  var clip = u.worldToClip * vec4<f32>(input.position, 1.0);\n"
        "  clip.z = clip.z * 0.5 + clip.w * 0.5;\n"
        "  return clip;\n"
        "}\n";
    WGPUShaderModule module = NULL;
    WGPUBindGroupLayout bgl = NULL;
    WGPUPipelineLayout layout = NULL;
    WGPURenderPipeline pipelines[2] = {NULL, NULL};
    WGPUBuffer matrix_ubo[GFX_SHADOW_MAX_MAPS] = {NULL};
    WGPUBindGroup matrix_bg[GFX_SHADOW_MAX_MAPS] = {NULL};
    WGPUBuffer receiver_ubo[GFX_SHADOW_MAX_VIEWS] = {NULL};

    WGPUShaderSourceWGSL wgsl = {0};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = wgpu_sv(source);
    WGPUShaderModuleDescriptor smd = {0};
    smd.nextInChain = (WGPUChainedStruct *)&wgsl;
    module = wgpuDeviceCreateShaderModule(s_device, &smd);
    if (module == NULL) goto fail;

    WGPUBindGroupLayoutEntry bgle = {0};
    bgle.binding = 0;
    bgle.visibility = WGPUShaderStage_Vertex;
    bgle.buffer.type = WGPUBufferBindingType_Uniform;
    bgle.buffer.minBindingSize = 64;
    WGPUBindGroupLayoutDescriptor bgld = {0};
    bgld.entryCount = 1;
    bgld.entries = &bgle;
    bgl = wgpuDeviceCreateBindGroupLayout(s_device, &bgld);
    if (bgl == NULL) goto fail;

    WGPUPipelineLayoutDescriptor pld = {0};
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &bgl;
    layout = wgpuDeviceCreatePipelineLayout(s_device, &pld);
    if (layout == NULL) goto fail;

    WGPUVertexAttribute attribute = {0};
    attribute.format = WGPUVertexFormat_Float32x3;
    attribute.offset = 0;
    attribute.shaderLocation = 0;
    WGPUVertexBufferLayout vertex_layout = {0};
    vertex_layout.arrayStride = sizeof(GfxShadowVertex);
    vertex_layout.stepMode = WGPUVertexStepMode_Vertex;
    vertex_layout.attributeCount = 1;
    vertex_layout.attributes = &attribute;

    WGPUDepthStencilState depth = {0};
    depth.format = WGPU_SHADOW_DEPTH_FORMAT;
    depth.depthWriteEnabled = WGPUOptionalBool_True;
    depth.depthCompare = WGPUCompareFunction_Less;
    depth.depthBias = 4;
    depth.depthBiasSlopeScale = 2.0f;
    depth.stencilFront.compare = WGPUCompareFunction_Always;
    depth.stencilFront.failOp = WGPUStencilOperation_Keep;
    depth.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
    depth.stencilFront.passOp = WGPUStencilOperation_Keep;
    depth.stencilBack = depth.stencilFront;

    WGPURenderPipelineDescriptor pd = {0};
    pd.layout = layout;
    pd.vertex.module = module;
    pd.vertex.entryPoint = wgpu_sv("vs_main");
    pd.vertex.bufferCount = 1;
    pd.vertex.buffers = &vertex_layout;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    /* Depth-clamp casters like GL's process-global GL_DEPTH_CLAMP does for
     * the shadow FBO pass: a caster outside the planned z-range must pancake
     * onto the near plane, not silently clip — the historical divergence
     * behind the v0.4 phantom-band asymmetry. Every other pipeline in this
     * file already sets this. */
    pd.primitive.unclippedDepth =
        s_unclipped_depth_supported ? WGPU_TRUE : WGPU_FALSE;
    pd.depthStencil = &depth;
    pd.multisample.count = 1;
    pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = NULL;
    for (size_t index = 0; index < 2; index++) {
        pd.primitive.cullMode =
            index == 0 ? WGPUCullMode_Front : WGPUCullMode_None;
        pipelines[index] =
            wgpuDeviceCreateRenderPipeline(s_device, &pd);
        if (pipelines[index] == NULL) goto fail;
    }

    for (size_t index = 0; index < GFX_SHADOW_MAX_MAPS; index++) {
        WGPUBufferDescriptor bd = {0};
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bd.size = 64;
        matrix_ubo[index] = wgpuDeviceCreateBuffer(s_device, &bd);
        if (matrix_ubo[index] == NULL) goto fail;

        WGPUBindGroupEntry entry = {0};
        entry.binding = 0;
        entry.buffer = matrix_ubo[index];
        entry.size = 64;
        WGPUBindGroupDescriptor bgd = {0};
        bgd.layout = bgl;
        bgd.entryCount = 1;
        bgd.entries = &entry;
        matrix_bg[index] = wgpuDeviceCreateBindGroup(s_device, &bgd);
        if (matrix_bg[index] == NULL) goto fail;
    }
    for (size_t index = 0; index < GFX_SHADOW_MAX_VIEWS; index++) {
        WGPUBufferDescriptor bd = {0};
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bd.size = WGPU_SHADOW_UNIFORM_SIZE;
        receiver_ubo[index] = wgpuDeviceCreateBuffer(s_device, &bd);
        if (receiver_ubo[index] == NULL) goto fail;
    }

    s_shadow_module = module;
    s_shadow_pass_bgl = bgl;
    s_shadow_pass_layout = layout;
    memcpy(s_shadow_pipeline, pipelines, sizeof(pipelines));
    memcpy(s_shadow_matrix_ubo, matrix_ubo, sizeof(matrix_ubo));
    memcpy(s_shadow_matrix_bg, matrix_bg, sizeof(matrix_bg));
    memcpy(s_shadow_receiver_ubo, receiver_ubo, sizeof(receiver_ubo));
    return true;

fail:
    for (size_t index = 0; index < GFX_SHADOW_MAX_MAPS; index++) {
        if (matrix_bg[index] != NULL) wgpuBindGroupRelease(matrix_bg[index]);
        if (matrix_ubo[index] != NULL) wgpuBufferRelease(matrix_ubo[index]);
    }
    for (size_t index = 0; index < GFX_SHADOW_MAX_VIEWS; index++) {
        if (receiver_ubo[index] != NULL) wgpuBufferRelease(receiver_ubo[index]);
    }
    for (size_t index = 0; index < 2; index++) {
        if (pipelines[index] != NULL) {
            wgpuRenderPipelineRelease(pipelines[index]);
        }
    }
    if (layout != NULL) wgpuPipelineLayoutRelease(layout);
    if (bgl != NULL) wgpuBindGroupLayoutRelease(bgl);
    if (module != NULL) wgpuShaderModuleRelease(module);
    return false;
}

static bool wgpu_shadow_ensure_texture_set(
    uint32_t resolution,
    uint32_t layers) {
    if (s_shadow_tex != NULL &&
        s_shadow_array_view != NULL &&
        s_shadow_sampler != NULL &&
        s_shadow_res == resolution &&
        s_shadow_layers == layers) {
        return true;
    }
    if (resolution == 0 || layers == 0 ||
        layers > GFX_SHADOW_MAX_MAPS) {
        return false;
    }

    WGPUTexture texture = NULL;
    WGPUTextureView array_view = NULL;
    WGPUTextureView layer_view[GFX_SHADOW_MAX_MAPS] = {NULL};
    WGPUSampler sampler = NULL;

    WGPUTextureDescriptor td = {0};
    td.usage =
        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = resolution;
    td.size.height = resolution;
    td.size.depthOrArrayLayers = layers;
    td.format = WGPU_SHADOW_DEPTH_FORMAT;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    texture = wgpuDeviceCreateTexture(s_device, &td);
    if (texture == NULL) goto fail;

    WGPUTextureViewDescriptor avd = {0};
    avd.format = WGPU_SHADOW_DEPTH_FORMAT;
    avd.dimension = WGPUTextureViewDimension_2DArray;
    avd.baseMipLevel = 0;
    avd.mipLevelCount = 1;
    avd.baseArrayLayer = 0;
    avd.arrayLayerCount = layers;
    avd.aspect = WGPUTextureAspect_DepthOnly;
    array_view = wgpuTextureCreateView(texture, &avd);
    if (array_view == NULL) goto fail;

    for (uint32_t index = 0; index < layers; index++) {
        WGPUTextureViewDescriptor lvd = avd;
        lvd.dimension = WGPUTextureViewDimension_2D;
        lvd.baseArrayLayer = index;
        lvd.arrayLayerCount = 1;
        layer_view[index] = wgpuTextureCreateView(texture, &lvd);
        if (layer_view[index] == NULL) goto fail;
    }

    WGPUSamplerDescriptor sd = {0};
    sd.addressModeU = WGPUAddressMode_ClampToEdge;
    sd.addressModeV = WGPUAddressMode_ClampToEdge;
    sd.addressModeW = WGPUAddressMode_ClampToEdge;
    sd.magFilter = WGPUFilterMode_Linear;
    sd.minFilter = WGPUFilterMode_Linear;
    sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sd.compare = WGPUCompareFunction_LessEqual;
    sd.maxAnisotropy = 1;
    sampler = wgpuDeviceCreateSampler(s_device, &sd);
    if (sampler == NULL) goto fail;

    wgpu_shadow_release_texture_set();
    s_shadow_tex = texture;
    s_shadow_array_view = array_view;
    memcpy(s_shadow_layer_view, layer_view, sizeof(layer_view));
    s_shadow_sampler = sampler;
    s_shadow_res = resolution;
    s_shadow_layers = layers;
    return true;

fail:
    for (size_t index = 0; index < GFX_SHADOW_MAX_MAPS; index++) {
        if (layer_view[index] != NULL) {
            wgpuTextureViewRelease(layer_view[index]);
        }
    }
    if (array_view != NULL) wgpuTextureViewRelease(array_view);
    if (texture != NULL) wgpuTextureRelease(texture);
    if (sampler != NULL) wgpuSamplerRelease(sampler);
    return false;
}

static bool wgpu_shadow_ensure_vbuf(uint64_t bytes) {
    if (bytes == 0) return false;
    if (s_shadow_vbuf != NULL && s_shadow_vbuf_cap >= bytes) {
        return true;
    }
    uint64_t capacity = s_shadow_vbuf_cap > 0 ? s_shadow_vbuf_cap : 65536;
    while (capacity < bytes) {
        if (capacity > UINT64_MAX / 2) return false;
        capacity *= 2;
    }
    WGPUBufferDescriptor bd = {0};
    bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    bd.size = capacity;
    WGPUBuffer next = wgpuDeviceCreateBuffer(s_device, &bd);
    if (next == NULL) return false;
    if (s_shadow_vbuf != NULL) wgpuBufferRelease(s_shadow_vbuf);
    s_shadow_vbuf = next;
    s_shadow_vbuf_cap = capacity;
    return true;
}

static size_t wgpu_shadow_draw_ranges(
    WGPURenderPassEncoder pass,
    const GfxShadowRange *ranges,
    size_t range_count,
    size_t base_vertex,
    size_t segment_vertices,
    uint8_t view_index,
    uint64_t total_vertices) {
    size_t triangles = 0;
    int pipeline_index = -1;
    for (size_t index = 0; index < range_count; index++) {
        const GfxShadowRange *range = &ranges[index];
        if ((range->view_index != UINT8_MAX &&
             range->view_index != view_index) ||
            range->alpha_mode != GFX_SHADOW_ALPHA_OPAQUE ||
            range->vertex_count == 0 ||
            range->first_vertex > segment_vertices ||
            range->vertex_count >
                segment_vertices - range->first_vertex ||
            range->first_vertex > SIZE_MAX - base_vertex) {
            continue;
        }
        size_t first = base_vertex + range->first_vertex;
        if (first > UINT32_MAX ||
            range->vertex_count > UINT32_MAX ||
            (uint64_t)first + range->vertex_count > total_vertices) {
            continue;
        }
        /* No culling for any caster — 94% of level triangles are
         * single-sided open shell (see the GL depth pass for the corpus
         * numbers); acne is owned by the world-unit bias. Pipeline 1 is the
         * CullMode_None variant. */
        int next_pipeline = 1;
        if (next_pipeline != pipeline_index) {
            wgpuRenderPassEncoderSetPipeline(
                pass, s_shadow_pipeline[next_pipeline]);
            pipeline_index = next_pipeline;
        }
        wgpuRenderPassEncoderDraw(
            pass, (uint32_t)range->vertex_count, 1,
            (uint32_t)first, 0);
        triangles += range->vertex_count / 3;
    }
    return triangles;
}

static void wgpu_shadow_upload_receiver(size_t view_index) {
    float uniform[WGPU_SHADOW_UNIFORM_SIZE / sizeof(float)] = {0};
    /* The uniform holds exactly GFX_SHADOW_MAX_CASCADES matrices; a budget that
     * planned more would pack over the params tail. */
    size_t cascade_count = s_shadow_plan.budget.cascades_per_view;
    size_t base;
    if (cascade_count > GFX_SHADOW_MAX_CASCADES) {
        cascade_count = GFX_SHADOW_MAX_CASCADES;
    }
    base = view_index * (size_t)s_shadow_plan.budget.cascades_per_view;
    for (size_t cascade = 0; cascade < cascade_count; cascade++) {
        wgpu_shadow_pack_matrix(
            &uniform[cascade * 16],
            s_shadow_plan.cascades[base + cascade].world_to_clip);
    }
    if (cascade_count == 1) {
        memcpy(&uniform[16], &uniform[0], 16 * sizeof(float));
    }
    uniform[32] =
        s_shadow_res > 0 ? 1.0f / (float)s_shadow_res : 1.0f / 2048.0f;
    /* World-unit bias normalized by the planned light z-span; mirrors the GL
     * receiver exactly (see gfx_opengl.c's uniform site for the rationale). */
    {
        float span = 0.0f;
        float bias_ndc = 0.002f;
        for (size_t cascade = 0; cascade < cascade_count; cascade++) {
            const GfxShadowCascade *c =
                &s_shadow_plan.cascades[base + cascade];
            float s = c->light_bounds_max[2] - c->light_bounds_min[2];
            if (s > span) {
                span = s;
            }
        }
        if (span > 1.0f) {
            bias_ndc = g_pcSunShadowBias / span;
            if (bias_ndc < 0.0002f) bias_ndc = 0.0002f;
            if (bias_ndc > 0.01f) bias_ndc = 0.01f;
        }
        uniform[33] = bias_ndc;
    }
    uniform[34] = g_pcSunShadowUmbra;
    uniform[35] = (float)cascade_count;
    if (cascade_count > 1) {
        uniform[36] = s_shadow_plan.cascades[base + 1].split_near;
        uniform[37] = s_shadow_plan.cascades[base].split_far;
    }
    uniform[38] = (float)s_shadow_plan.cascades[base].map_index;
    uniform[39] = (float)s_shadow_plan
        .cascades[base + (cascade_count > 1 ? 1 : 0)].map_index;
    wgpuQueueWriteBuffer(
        s_queue, s_shadow_receiver_ubo[view_index], 0,
        uniform, sizeof(uniform));
}

static void wgpu_render_shadow_maps(void) {
    const GfxShadowFrame *frame;
    size_t static_bytes;
    size_t dynamic_bytes;
    size_t total_bytes;
    bool view_complete[GFX_SHADOW_MAX_VIEWS] = {true, true, true, true};
    uint32_t complete_mask = 0;
    static int previous_enabled = 0;

    g_pc_shadow_map_ready = 0;
    g_pc_shadow_mat_valid = 0;
    g_pc_shadow_view_ready_mask = 0;
    memset(&s_shadow_plan, 0, sizeof(s_shadow_plan));
    s_shadow_receiver_view = -1;
    if (g_pcSunShadow && !previous_enabled) {
        s_shadow_resource_fail_count = 0;
#ifdef __EMSCRIPTEN__
        if (!s_shadow_receiver_prewarm_failed) {
            s_shadow_resource_perma_fail = false;
        }
#else
        s_shadow_resource_perma_fail = false;
#endif
    }
    previous_enabled = g_pcSunShadow;
    if (!g_pcSunShadow || s_encoder == NULL) return;
    s_shadow_attempted_frames++;

    frame = gfx_shadow_frame_previous();
    if (!gfx_shadow_build_plan(
            frame, g_pc_sun_dir_world, &s_shadow_plan)) {
        s_shadow_fallback_frames++;
        return;
    }
    if (!wgpu_shadow_resource_retry_allowed(
            s_shadow_plan.budget.resolution,
            s_shadow_plan.budget.map_count)) {
        s_shadow_fallback_frames++;
        memset(&s_shadow_plan, 0, sizeof(s_shadow_plan));
        return;
    }
    if (getenv("MDKR_TEST_WORLD_SHADOW_RESOURCE_FAIL") != NULL) {
        wgpu_shadow_resource_fail(
            s_shadow_plan.budget.resolution,
            s_shadow_plan.budget.map_count,
            "forced-test-failure");
        s_shadow_fallback_frames++;
        memset(&s_shadow_plan, 0, sizeof(s_shadow_plan));
        return;
    }
    if (!wgpu_shadow_ensure_pipeline_resources() ||
        !wgpu_shadow_ensure_texture_set(
            s_shadow_plan.budget.resolution,
            s_shadow_plan.budget.map_count)) {
        wgpu_shadow_resource_fail(
            s_shadow_plan.budget.resolution,
            s_shadow_plan.budget.map_count,
            "pipeline-or-depth-texture");
        s_shadow_fallback_frames++;
        memset(&s_shadow_plan, 0, sizeof(s_shadow_plan));
        return;
    }
    if (frame->static_vertex_count > SIZE_MAX / sizeof(GfxShadowVertex) ||
        frame->vertex_count > SIZE_MAX / sizeof(GfxShadowVertex) ||
        (frame->static_vertex_count > 0 &&
         frame->static_vertices == NULL) ||
        (frame->vertex_count > 0 && frame->vertices == NULL) ||
        (frame->static_range_count > 0 &&
         frame->static_ranges == NULL) ||
        (frame->range_count > 0 && frame->ranges == NULL)) {
        s_shadow_fallback_frames++;
        memset(&s_shadow_plan, 0, sizeof(s_shadow_plan));
        return;
    }
    static_bytes =
        frame->static_vertex_count * sizeof(GfxShadowVertex);
    dynamic_bytes = frame->vertex_count * sizeof(GfxShadowVertex);
    if (static_bytes > SIZE_MAX - dynamic_bytes) {
        s_shadow_fallback_frames++;
        memset(&s_shadow_plan, 0, sizeof(s_shadow_plan));
        return;
    }
    total_bytes = static_bytes + dynamic_bytes;
    if (total_bytes == 0 ||
        !wgpu_shadow_ensure_vbuf((uint64_t)total_bytes)) {
        if (total_bytes != 0) {
            wgpu_shadow_resource_fail(
                s_shadow_plan.budget.resolution,
                s_shadow_plan.budget.map_count,
                "vertex-buffer");
        }
        s_shadow_fallback_frames++;
        memset(&s_shadow_plan, 0, sizeof(s_shadow_plan));
        return;
    }
    if (total_bytes > s_shadow_upload_cap) {
        uint8_t *next = (uint8_t *)realloc(s_shadow_upload, total_bytes);
        if (next == NULL) {
            wgpu_shadow_resource_fail(
                s_shadow_plan.budget.resolution,
                s_shadow_plan.budget.map_count,
                "cpu-staging");
            s_shadow_fallback_frames++;
            memset(&s_shadow_plan, 0, sizeof(s_shadow_plan));
            return;
        }
        s_shadow_upload = next;
        s_shadow_upload_cap = total_bytes;
    }
    if (static_bytes > 0) {
        memcpy(s_shadow_upload, frame->static_vertices, static_bytes);
    }
    if (dynamic_bytes > 0) {
        memcpy(
            s_shadow_upload + static_bytes,
            frame->vertices, dynamic_bytes);
    }
    wgpuQueueWriteBuffer(
        s_queue, s_shadow_vbuf, 0, s_shadow_upload, total_bytes);

    for (size_t index = 0;
         index < s_shadow_plan.cascade_count;
         index++) {
        const GfxShadowCascade *cascade =
            &s_shadow_plan.cascades[index];
        float matrix[16];
        size_t drawn = 0;
        wgpu_shadow_pack_matrix(matrix, cascade->world_to_clip);
        wgpuQueueWriteBuffer(
            s_queue, s_shadow_matrix_ubo[index], 0,
            matrix, sizeof(matrix));

        WGPURenderPassDepthStencilAttachment depth = {0};
        depth.view = s_shadow_layer_view[cascade->map_index];
        depth.depthLoadOp = WGPULoadOp_Clear;
        depth.depthStoreOp = WGPUStoreOp_Store;
        depth.depthClearValue = 1.0f;
        WGPURenderPassDescriptor rp = {0};
        rp.depthStencilAttachment = &depth;
        WGPURenderPassEncoder pass =
            wgpuCommandEncoderBeginRenderPass(s_encoder, &rp);
        if (pass == NULL) {
            view_complete[cascade->view_index] = false;
            continue;
        }
        wgpuRenderPassEncoderSetViewport(
            pass, 0.0f, 0.0f,
            (float)s_shadow_res, (float)s_shadow_res,
            0.0f, 1.0f);
        wgpuRenderPassEncoderSetScissorRect(
            pass, 0, 0, s_shadow_res, s_shadow_res);
        wgpuRenderPassEncoderSetBindGroup(
            pass, 0, s_shadow_matrix_bg[index], 0, NULL);
        wgpuRenderPassEncoderSetVertexBuffer(
            pass, 0, s_shadow_vbuf, 0, total_bytes);
        drawn += wgpu_shadow_draw_ranges(
            pass,
            frame->static_ranges,
            frame->static_range_count,
            0,
            frame->static_vertex_count,
            cascade->view_index,
            frame->static_vertex_count + frame->vertex_count);
        drawn += wgpu_shadow_draw_ranges(
            pass,
            frame->ranges,
            frame->range_count,
            frame->static_vertex_count,
            frame->vertex_count,
            cascade->view_index,
            frame->static_vertex_count + frame->vertex_count);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        if (drawn == 0) {
            view_complete[cascade->view_index] = false;
        }
    }

    for (size_t view = 0; view < s_shadow_plan.view_count; view++) {
        if (view_complete[view]) {
            wgpu_shadow_upload_receiver(view);
            complete_mask |= 1u << view;
        }
    }
    if (wgpu_shadow_prewarm_receivers()) {
        g_pc_shadow_view_ready_mask = complete_mask;
    }
    g_pc_shadow_map_ready =
        g_pc_shadow_view_ready_mask ==
        ((1u << s_shadow_plan.view_count) - 1u);
    g_pc_shadow_mat_valid = g_pc_shadow_map_ready;
    if (g_pc_shadow_map_ready) {
        s_shadow_complete_frames++;
        s_shadow_resource_fail_count = 0;
    } else {
        s_shadow_fallback_frames++;
    }
    if (s_shadow_plan.cascade_count > 0) {
        memcpy(
            g_pc_shadow_mat,
            s_shadow_plan.cascades[0].world_to_clip,
            sizeof(g_pc_shadow_mat));
    }
}

static struct ShaderProgram *wgpu_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    for (size_t i = 0; i < s_shader_count; ++i) {
        if (s_shaders[i]->shader_id0 == shader_id0 &&
            s_shaders[i]->shader_id1 == shader_id1) {
            return s_shaders[i];
        }
    }
    return NULL;
}

static struct ShaderProgram *wgpu_alloc_shader(void) {
    const size_t shader_limit = wgpu_shader_effective_limit();
    if (s_shader_count >= shader_limit) {
        s_shader_guard_hits++;
        fprintf(stderr,
                "[webgpu] shader hard limit reached "
                "(effective=%zu hard=%u); refusing unsafe pointer reuse\n",
                shader_limit, WGPU_SHADER_HARD_LIMIT);
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend exceeded its shader safety budget. Reload "
            "the page to continue from the last persisted save.");
        return NULL;
    }
    if (s_shader_count == s_shader_capacity) {
        size_t new_capacity = s_shader_capacity ? s_shader_capacity * 2u : 128u;
        if (new_capacity > shader_limit) {
            new_capacity = shader_limit;
        }
        struct ShaderProgram **grown = (struct ShaderProgram **)realloc(
            s_shaders, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            fprintf(stderr,
                    "[webgpu] could not grow the shader index to %zu entries\n",
                    new_capacity);
            s_ready = false;
            s_runtime_status = GFX_RENDERING_FATAL;
            WGPU_COMPAT_REPORT_FAILURE(
                "The graphics backend ran out of memory while indexing "
                "shaders. Reload the page to continue from the last persisted "
                "save.");
            return NULL;
        }
        s_shaders = grown;
        s_shader_capacity = new_capacity;
    }
    struct ShaderProgram *prg =
        (struct ShaderProgram *)calloc(1, sizeof(*prg));
    if (prg == NULL) {
        fprintf(stderr, "[webgpu] shader allocation failed\n");
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend ran out of memory while creating a shader. "
            "Reload the page to continue from the last persisted save.");
        return NULL;
    }
    s_shaders[s_shader_count++] = prg;
    if (s_shader_count > s_shader_high_water) {
        s_shader_high_water = s_shader_count;
    }
    return prg;
}

/* Build the bind-group layout for a combiner: (tex,sampler) pairs for each used
 * texture at bindings (0,1) and (2,3). NULL when no textures are sampled. */
static WGPUBindGroupLayout wgpu_make_bgl(const struct WgpuShaderInfo *info) {
    WGPUBindGroupLayoutEntry ents[12];
    int ne = 0;
    for (int t = 0; t < 2; t++) {
        if (!info->used_textures[t]) continue;
        WGPUBindGroupLayoutEntry te = {0};
        te.binding = (uint32_t)(t * 2);
        te.visibility = WGPUShaderStage_Fragment;
        te.texture.sampleType = WGPUTextureSampleType_Float;
        te.texture.viewDimension = WGPUTextureViewDimension_2D;
        ents[ne++] = te;
        WGPUBindGroupLayoutEntry se = {0};
        se.binding = (uint32_t)(t * 2 + 1);
        se.visibility = WGPUShaderStage_Fragment;
        se.sampler.type = WGPUSamplerBindingType_Filtering;
        ents[ne++] = se;
    }
    if (info->diag_rdp_memory_blend || info->diag_rdp_cvg_memory_blend) {
        WGPUBindGroupLayoutEntry te = {0};
        te.binding = 4;   /* scene snapshot ("memory color") */
        te.visibility = WGPUShaderStage_Fragment;
        te.texture.sampleType = WGPUTextureSampleType_Float;
        te.texture.viewDimension = WGPUTextureViewDimension_2D;
        ents[ne++] = te;
        WGPUBindGroupLayoutEntry se = {0};
        se.binding = 5;
        se.visibility = WGPUShaderStage_Fragment;
        se.sampler.type = WGPUSamplerBindingType_Filtering;
        ents[ne++] = se;
    }
    if (info->diag_rdp_cvg_memory_blend) {
        WGPUBindGroupLayoutEntry ue = {0};
        ue.binding = 6;   /* GL-convention viewport (uDiagViewport) */
        ue.visibility = WGPUShaderStage_Fragment;
        ue.buffer.type = WGPUBufferBindingType_Uniform;
        ue.buffer.minBindingSize = 16;
        ents[ne++] = ue;
    }
    /* WEB-027: per-frame noise uniform, only for combiners that read SHADER_NOISE
     * (keeps noise-free pipelines at their exact prior binding set). */
    if (info->uses_noise) {
        WGPUBindGroupLayoutEntry ne_ent = {0};
        ne_ent.binding = 7;   /* uNoise (frame counter + render height) */
        ne_ent.visibility = WGPUShaderStage_Fragment;
        ne_ent.buffer.type = WGPUBufferBindingType_Uniform;
        ne_ent.buffer.minBindingSize = 16;
        ents[ne++] = ne_ent;
    }
    if (info->opt_dfdx_light) {
        WGPUBindGroupLayoutEntry light = {0};
        light.binding = 8;
        light.visibility = WGPUShaderStage_Fragment;
        light.buffer.type = WGPUBufferBindingType_Uniform;
        light.buffer.minBindingSize = 16;
        ents[ne++] = light;
    }
    if (info->opt_sun_shadow) {
        WGPUBindGroupLayoutEntry depth = {0};
        depth.binding = 9;
        depth.visibility = WGPUShaderStage_Fragment;
        depth.texture.sampleType = WGPUTextureSampleType_Depth;
        depth.texture.viewDimension = WGPUTextureViewDimension_2DArray;
        ents[ne++] = depth;

        WGPUBindGroupLayoutEntry compare = {0};
        compare.binding = 10;
        compare.visibility = WGPUShaderStage_Fragment;
        compare.sampler.type = WGPUSamplerBindingType_Comparison;
        ents[ne++] = compare;

        WGPUBindGroupLayoutEntry shadow = {0};
        shadow.binding = 11;
        shadow.visibility = WGPUShaderStage_Fragment;
        shadow.buffer.type = WGPUBufferBindingType_Uniform;
        shadow.buffer.minBindingSize = WGPU_SHADOW_UNIFORM_SIZE;
        ents[ne++] = shadow;
    }
    if (ne == 0) {
        return NULL;
    }
    WGPUBindGroupLayoutDescriptor d = {0};
    d.entryCount = (size_t)ne;
    d.entries = ents;
    return WGPU_FAULT_CREATE(
        SHADER_BGL, wgpuDeviceCreateBindGroupLayout(s_device, &d));
}

static struct ShaderProgram *wgpu_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    if (!s_ready || s_runtime_status == GFX_RENDERING_FATAL) {
        s_cur_shader = NULL;
        return NULL;
    }
    struct ShaderProgram *prg = wgpu_lookup_shader(shader_id0, shader_id1);
    if (prg != NULL) {
        s_cur_shader = prg;
        return prg;
    }
    prg = wgpu_alloc_shader();
    if (prg == NULL) {
        s_cur_shader = NULL;
        return NULL;
    }
    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;

    char *wgsl = gfx_webgpu_build_wgsl(shader_id0, shader_id1, &prg->info);
    if (wgsl == NULL || !s_ready) {
        free(wgsl);
        fprintf(stderr,
                "[webgpu] WGSL generation failed for shader "
                "%016llx/%08x\n",
                (unsigned long long)shader_id0, (unsigned)shader_id1);
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend could not generate a required shader. "
            "Reload the page to continue from the last persisted save.");
        s_cur_shader = NULL;
        return NULL;
    }

    /*
     * Validate generated metadata before copying it into fixed storage or
     * asking the driver to compile a module. The emitter has a wider defensive
     * capacity than WebGPU's portable guaranteed limits; neither boundary may
     * be crossed speculatively.
     */
    {
        int n_attrs = prg->info.num_attrs;
        int n_vary =
            (n_attrs > 0 ? n_attrs - 1 : 0) +
            (prg->info.opt_sun_shadow ? 1 : 0);
        bool metadata_valid =
            n_attrs > 0 &&
            n_attrs <= MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY &&
            /*
             * WebGPU's portable maxVertexBufferArrayStride is 2048 bytes.
             * Staying inside it also makes every offset/size sum below safe
             * from integer overflow in the checks that follow.
             */
            prg->info.num_floats > 0 &&
            prg->info.num_floats <= 2048 / (int)sizeof(float);
        uint32_t used_locations = 0;
        for (int i = 0; metadata_valid && i < n_attrs; i++) {
            int location = prg->info.attrs[i].location;
            int size = prg->info.attrs[i].size;
            int offset = prg->info.attrs[i].offset;
            metadata_valid =
                size >= 1 && size <= 4 &&
                location >= 0 && location < 16 &&
                (used_locations & (1u << (unsigned)location)) == 0 &&
                offset >= 0 &&
                offset <= prg->info.num_floats - size;
            if (metadata_valid) {
                used_locations |= 1u << (unsigned)location;
            }
        }
        if (n_attrs > s_max_attrs_seen) {
            s_max_attrs_seen = n_attrs;
        }
        if (n_vary > s_max_varyings_seen) {
            s_max_varyings_seen = n_vary;
        }
        if (!metadata_valid || n_attrs > 16 || n_vary > 16) {
            fprintf(stderr,
                    "[webgpu] invalid or non-portable combiner layout "
                    "(attrs=%d, varyings=%d, floats=%d; portable max 16) "
                    "shader id=%016llx/%08x\n",
                    n_attrs, n_vary, prg->info.num_floats,
                    (unsigned long long)shader_id0,
                    (unsigned)shader_id1);
            fflush(stderr);
            free(wgsl);
            s_ready = false;
            s_runtime_status = GFX_RENDERING_FATAL;
            WGPU_COMPAT_REPORT_FAILURE(
                "A required material exceeds the graphics device's portable "
                "shader limits. Reload the page to continue from the last "
                "persisted save.");
            s_cur_shader = NULL;
            return NULL;
        }
    }

    WGPUShaderSourceWGSL src = {0};
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = wgpu_sv(wgsl);
    WGPUShaderModuleDescriptor smd = {0};
    smd.nextInChain = (WGPUChainedStruct *)&src;
    prg->module = WGPU_FAULT_CREATE(
        SHADER_MODULE, wgpuDeviceCreateShaderModule(s_device, &smd));
    free(wgsl);
    if (prg->module == NULL) {
        fprintf(stderr,
                "[webgpu] shader-module creation failed for "
                "%016llx/%08x\n",
                (unsigned long long)shader_id0, (unsigned)shader_id1);
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend could not compile a required shader. "
            "Reload the page to continue from the last persisted save.");
        s_cur_shader = NULL;
        return NULL;
    }

    /* Vertex attributes for the pipeline's WGPUVertexBufferLayout. */
    for (int i = 0; i < prg->info.num_attrs; i++) {
        int sz = prg->info.attrs[i].size;
        prg->vattrs[i].format = sz == 1 ? WGPUVertexFormat_Float32
                              : sz == 2 ? WGPUVertexFormat_Float32x2
                              : sz == 3 ? WGPUVertexFormat_Float32x3
                                        : WGPUVertexFormat_Float32x4;
        prg->vattrs[i].offset = (uint64_t)prg->info.attrs[i].offset * sizeof(float);
        prg->vattrs[i].shaderLocation = (uint32_t)prg->info.attrs[i].location;
    }

    bool needs_bgl =
        prg->info.used_textures[0] || prg->info.used_textures[1] ||
        prg->info.diag_rdp_memory_blend ||
        prg->info.diag_rdp_cvg_memory_blend || prg->info.uses_noise ||
        prg->info.opt_dfdx_light || prg->info.opt_sun_shadow;
    prg->bgl = wgpu_make_bgl(&prg->info);
    if (needs_bgl && prg->bgl == NULL) {
        fprintf(stderr,
                "[webgpu] bind-group-layout creation failed for shader "
                "%016llx/%08x\n",
                (unsigned long long)shader_id0, (unsigned)shader_id1);
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend could not allocate a required material "
            "layout. Reload the page to continue from the last persisted save.");
        s_cur_shader = NULL;
        return NULL;
    }
    WGPUPipelineLayoutDescriptor pld = {0};
    if (prg->bgl != NULL) {
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &prg->bgl;
    }
    prg->playout = WGPU_FAULT_CREATE(
        SHADER_LAYOUT, wgpuDeviceCreatePipelineLayout(s_device, &pld));
    if (prg->playout == NULL) {
        fprintf(stderr,
                "[webgpu] pipeline-layout creation failed for shader "
                "%016llx/%08x\n",
                (unsigned long long)shader_id0, (unsigned)shader_id1);
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend could not allocate a required pipeline "
            "layout. Reload the page to continue from the last persisted save.");
        s_cur_shader = NULL;
        return NULL;
    }

    s_cur_shader = prg;
    return prg;
}

static void wgpu_load_shader(struct ShaderProgram *new_prg) { s_cur_shader = new_prg; }
static void wgpu_unload_shader(struct ShaderProgram *old_prg) {
    (void)old_prg;
    s_cur_shader = NULL;
}

static void wgpu_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    if (prg != NULL) {
        if (num_inputs != NULL) *num_inputs = (uint8_t)prg->info.num_inputs;
        if (used_textures != NULL) {
            used_textures[0] = prg->info.used_textures[0];
            used_textures[1] = prg->info.used_textures[1];
        }
    } else {
        if (num_inputs != NULL) *num_inputs = 0;
        if (used_textures != NULL) { used_textures[0] = false; used_textures[1] = false; }
    }
}

/* Map a GfxBlendMode to a WGPUBlendState, matching gfx_opengl_set_blend_mode's
 * default (diag mode 0) exactly. glBlendFunc applies the same (src,dst) factors
 * to BOTH the color and alpha channels, so both components use identical factors
 * here. Returns false for the opaque modes (no blend state attached):
 *   - DISABLED, and the two RDP-memory modes (GL disables HW blend for those —
 *     their blending is shader-side framebuffer sampling, a diag path not yet
 *     ported; the opaque HW state still matches GL).
 *   - MODULATE -> (DST_COLOR, ZERO)  = src*dst.
 *   - ALPHA / ALPHA_COVERAGE / ALPHA_CVG_WRAP_STENCIL -> (SRC_ALPHA, 1-SRC_ALPHA). */
static bool wgpu_blend_state(enum GfxBlendMode mode, WGPUBlendState *out) {
    memset(out, 0, sizeof(*out));
    if (mode == GFX_BLEND_DISABLED ||
        mode == GFX_BLEND_ALPHA_RDP_MEMORY ||
        mode == GFX_BLEND_ALPHA_RDP_CVG_MEMORY) {
        return false;   /* opaque */
    }
    WGPUBlendFactor src, dst;
    if (mode == GFX_BLEND_MODULATE) {
        src = WGPUBlendFactor_Dst;      /* GL_DST_COLOR */
        dst = WGPUBlendFactor_Zero;     /* GL_ZERO */
    } else {                            /* ALPHA + coverage/stencil variants */
        src = WGPUBlendFactor_SrcAlpha;
        dst = WGPUBlendFactor_OneMinusSrcAlpha;
    }
    out->color.operation = WGPUBlendOperation_Add;
    out->color.srcFactor = src;
    out->color.dstFactor = dst;
    out->alpha.operation = WGPUBlendOperation_Add;
    out->alpha.srcFactor = src;
    out->alpha.dstFactor = dst;
    return true;
}

/* BLEND-1: coverage-alpha preservation, mirroring gfx_opengl.c:1905 and
 * gfx_metal.mm:3368. The RDP coverage-memory path stores a synthetic 3-bit
 * coverage in the scene-target ALPHA channel; a later draw reads it back to
 * emulate N64 CVG_DST_WRAP. When that feature is active, ordinary translucent
 * draws interleaved between two cvg-memory draws must NOT overwrite the stored
 * coverage — GL masks alpha off (glColorMask(T,T,T,FALSE)), Metal drops it into
 * the PSO colorWriteMask, and WebGPU bakes an RGB-only writeMask into the
 * pipeline via the key (below). The WebGPU scene is ALWAYS rendered into the
 * offscreen s_scene_tex (== GL's scene target, bound-by-default because
 * room_xlu_cvg_memory defaults on), so — exactly like Metal — GL's
 * g_scene_target_bound term is unconditionally true here and folded out. */
static bool wgpu_room_cvg_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *d = getenv("GE007_DISABLE_ROOM_XLU_CVG_MEMORY");
        const char *e = getenv("GE007_ROOM_XLU_CVG_MEMORY");
        cached = 1;
        if ((d != NULL && d[0] != '\0' && d[0] != '0') || (e != NULL && e[0] == '0')) cached = 0;
    }
    return cached != 0;
}
static bool wgpu_diag_cvg_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("GE007_DIAG_XLU_RDP_CVG_MEMORY_BLEND_CC");
        cached = (e != NULL && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return cached != 0;
}
/* The exact GL predicate (gfx_opengl.c:1906-1913) modulo the always-true
 * scene-target term: preserve the coverage alpha for the four ordinary
 * translucent modes GL masks — but NOT for the RDP-memory / RDP-CVG-memory modes
 * themselves, which must WRITE the coverage they compute. A pure function of the
 * blend mode plus the session-constant env flags, so encoding it into the
 * pipeline key never splits a cache slot beyond what its blend bits already do. */
static bool wgpu_preserve_cov_alpha(enum GfxBlendMode blend) {
    return (wgpu_room_cvg_enabled() || wgpu_diag_cvg_enabled()) &&
           (blend == GFX_BLEND_ALPHA || blend == GFX_BLEND_MODULATE ||
            blend == GFX_BLEND_ALPHA_COVERAGE ||
            blend == GFX_BLEND_ALPHA_CVG_WRAP_STENCIL);
}

/* Scratch backing store for the pointer members of a WGPURenderPipelineDescriptor.
 * The descriptor holds raw pointers into these sub-structs, so they must outlive
 * the create call — the caller keeps one on its stack and hands it to
 * wgpu_fill_pipeline_desc by pointer (WebGPU consumes both synchronously). */
struct WgpuPipeScratch {
    WGPUVertexBufferLayout vbl;
    WGPUBlendState         blend;
    WGPUColorTargetState   color;
    WGPUFragmentState      fs;
    WGPUDepthStencilState  ds;
};

/* PERF-005: assemble the render-pipeline descriptor for `prg` at the explicit
 * 8-bit dynamic-state `key`, DECODING it (not reading the s_depth_* globals) so
 * the synchronous and asynchronous create paths build a BIT-IDENTICAL descriptor.
 * That identity is the #1 correctness guard: an async pipeline that differs from
 * what the draw would have built synchronously is a silent render divergence.
 * Everything else the descriptor reads (s_surface_format, s_unclipped_depth_supported,
 * prg->module/vattrs/info/playout) is stable config, identical on both paths.
 *
 * Key layout (see wgpu_pipeline_for): blend = bits 0-3, depth_test = bit 4,
 * depth_update = bit 5, depth_compare = bit 6, decal = bit 7,
 * preserve_cov_alpha = bit 8 (BLEND-1). */
static void wgpu_fill_pipeline_desc(struct ShaderProgram *prg, uint32_t key,
                                    WGPURenderPipelineDescriptor *pd,
                                    struct WgpuPipeScratch *sc) {
    enum GfxBlendMode blend = (enum GfxBlendMode)(key & 0xF);
    bool depth_test    = (key >> 4) & 1;
    bool depth_update  = (key >> 5) & 1;
    bool depth_compare = (key >> 6) & 1;
    bool decal         = (key >> 7) & 1;
    bool preserve_cov_alpha = (key >> 8) & 1;   /* BLEND-1: RGB-only writeMask */

    memset(sc, 0, sizeof(*sc));

    sc->vbl.stepMode = WGPUVertexStepMode_Vertex;
    sc->vbl.arrayStride = (uint64_t)prg->info.num_floats * sizeof(float);
    sc->vbl.attributeCount = (size_t)prg->info.num_attrs;
    sc->vbl.attributes = prg->vattrs;

    bool has_blend = wgpu_blend_state(blend, &sc->blend);
    sc->color.format = s_surface_format;
    /* BLEND-1: mask alpha off (RGB-only) so an interleaved ordinary XLU draw does
     * not clobber the stored RDP coverage-alpha; else write all four channels. */
    sc->color.writeMask = preserve_cov_alpha
        ? (WGPUColorWriteMask_Red | WGPUColorWriteMask_Green | WGPUColorWriteMask_Blue)
        : WGPUColorWriteMask_All;
    sc->color.blend = has_blend ? &sc->blend : NULL;

    sc->fs.module = prg->module;
    sc->fs.entryPoint = wgpu_sv("fs_main");
    sc->fs.targetCount = 1;
    sc->fs.targets = &sc->color;

    /* Depth: LessEqual when the N64 mode tests+compares (0..1 clip, 0 = near),
     * else Always; write when it tests+updates. Decal gets a small negative bias
     * so coplanar overlays win the test (mirrors GL glPolygonOffset(-2,-2)). */
    sc->ds.format = WGPU_DEPTH_FORMAT;
    sc->ds.depthCompare = (depth_test && depth_compare) ? WGPUCompareFunction_LessEqual
                                                        : WGPUCompareFunction_Always;
    sc->ds.depthWriteEnabled = (depth_test && depth_update)
                                   ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    if (decal) {
        sc->ds.depthBias = -2;
        sc->ds.depthBiasSlopeScale = -2.0f;
    }
    /* Depth-only format: stencil faces must be their default (Always/Keep) with
     * zero masks, or WebGPU rejects the pipeline. */
    sc->ds.stencilFront.compare = WGPUCompareFunction_Always;
    sc->ds.stencilFront.failOp = WGPUStencilOperation_Keep;
    sc->ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
    sc->ds.stencilFront.passOp = WGPUStencilOperation_Keep;
    sc->ds.stencilBack = sc->ds.stencilFront;

    memset(pd, 0, sizeof(*pd));
    pd->layout = prg->playout;
    pd->vertex.module = prg->module;
    pd->vertex.entryPoint = wgpu_sv("vs_main");
    pd->vertex.bufferCount = 1;
    pd->vertex.buffers = &sc->vbl;
    pd->primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd->primitive.frontFace = WGPUFrontFace_CCW;
    pd->primitive.cullMode = WGPUCullMode_None;   /* N64 backface handled on CPU */
    pd->primitive.unclippedDepth = s_unclipped_depth_supported ? WGPU_TRUE : WGPU_FALSE;
    pd->depthStencil = &sc->ds;
    pd->multisample.count = 1;
    pd->multisample.mask = 0xFFFFFFFFu;
    pd->fragment = &sc->fs;
}

/* Reserve a pipeline-cache slot for a new key: append while the cache has room,
 * else round-robin evict a completed entry. An asynchronous PENDING entry may
 * not be reused: its callback carries only (program,key), and evicting it would
 * orphan the completion or attach it to the wrong slot. Return -1 only if all
 * 32 slots are simultaneously pending; callers convert that impossible
 * pressure case into an explicit fatal state. */
static int wgpu_pipe_reserve_slot(struct ShaderProgram *prg) {
    int slot;
    if (prg->npipes < WGPU_PIPE_CACHE) {
        slot = prg->npipes++;
        s_pipeline_slots_live++;
        if (s_pipeline_slots_live > s_pipeline_slots_high_water) {
            s_pipeline_slots_high_water = s_pipeline_slots_live;
        }
    } else {
        slot = -1;
        for (int n = 0; n < WGPU_PIPE_CACHE; n++) {
            int candidate = (prg->pipe_evict + n) % WGPU_PIPE_CACHE;
            if (prg->pipes[candidate].state != WGPU_PIPE_PENDING) {
                slot = candidate;
                prg->pipe_evict = (candidate + 1) % WGPU_PIPE_CACHE;
                break;
            }
        }
        if (slot < 0) {
            return -1;
        }
        if (prg->pipes[slot].pipe != NULL) {
            wgpu_release_cached_pipeline(prg->pipes[slot].pipe);
        }
    }
    return slot;
}

/* ------------------------------------------------------------------------
 * PERF-005 Phase 2 (W4.3): pipeline record/replay prewarm.
 *
 * PERF-005 killed the synchronous first-sight compile stall by kicking pipeline
 * creates async (web-live) — but that traded the freeze for transient pop-in /
 * present-holds whenever the camera meets a COLD material (worst at level entry,
 * where EVERY material is cold). Phase 2 removes the cold set for revisits and
 * repeat sessions: RECORD the (shader_id0, shader_id1, pipe-key) tuples actually
 * used per stage, PERSIST them to a small text manifest in the save dir, and
 * REPLAY them SYNCHRONOUSLY during the next load screen (gfx_webgpu_prewarm_stage,
 * called from boss.c while the watchdog is suppressed and the load screen is up).
 * The async path stays the safety net for genuinely-new keys.
 *
 * CRUX (shader-creation-at-load): a recorded shader may not have a ShaderProgram
 * yet at load time (its first draw hasn't happened). That is fine — the shader is
 * built PURELY from the ids: wgpu_create_and_load_new_shader → gfx_webgpu_build_wgsl
 * → gfx_cc_get_features is a self-contained decode of (shader_id0, shader_id1) with
 * ZERO gfx_pc render state, so prewarm creates the module too. Full scope: a
 * persisted manifest warms everything from the second launch's first frame.
 *
 * Scope gate: record/persist/prewarm run only when the webgpu backend is the
 * active one and NOT under --deterministic (so every byte-exact gate stays on the
 * untouched synchronous HEAD path — prewarm never runs there). Persistence is
 * best-effort: any I/O failure just means "no prewarm", never fatal. */
extern int port_env_bool(const char *name, int default_on, const char *help); /* platform/port_env.h */

#define WGPU_PREWARM_FILE       "ge007_pipecache.txt"
#define WGPU_PREWARM_MAX        8192   /* total records across all stages */
#define WGPU_PREWARM_PER_STAGE  512    /* soft cap per stage (drop-on-full, one note) */

struct WgpuPrewarmRec { uint64_t id0; uint32_t id1; uint32_t key; int stage; };
static struct WgpuPrewarmRec s_prewarm_recs[WGPU_PREWARM_MAX];
static int  s_prewarm_n = 0;
static int  s_prewarm_cur_stage = -1;   /* stage the recorder tags new keys with */
static bool s_prewarm_dirty = false;    /* current stage's set grew since last flush */
static bool s_prewarm_loaded = false;   /* manifest read from disk once */

/* Enabled iff not deterministic and not explicitly disabled. Memoized: both
 * inputs are settled by the time the recorder first runs (argv already parsed),
 * and this is on the per-create record hot-ish path. */
static int wgpu_prewarm_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        extern int g_deterministic;
        cached = (!g_deterministic &&
                  port_env_bool("GE007_PIPECACHE", 1,
                      "PERF-005 Phase 2 record/replay pipeline prewarm (0 = off)"))
                 ? 1 : 0;
    }
    return cached;
}

/* Add (stage,id0,id1,key) if new and under the caps. Returns 1 when a record was
 * appended, 0 on duplicate or cap. Shared by disk-load and live-record. */
static int wgpu_prewarm_intern(int stage, uint64_t id0, uint32_t id1, uint32_t key) {
    int stage_count = 0;
    for (int i = 0; i < s_prewarm_n; i++) {
        if (s_prewarm_recs[i].stage != stage) continue;
        if (s_prewarm_recs[i].id0 == id0 && s_prewarm_recs[i].id1 == id1 &&
            s_prewarm_recs[i].key == key) {
            return 0;   /* already recorded */
        }
        stage_count++;
    }
    if (stage_count >= WGPU_PREWARM_PER_STAGE || s_prewarm_n >= WGPU_PREWARM_MAX) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr, "[webgpu] pipecache full (stage %d) - extra materials "
                            "won't prewarm\n", stage);
        }
        return 0;
    }
    s_prewarm_recs[s_prewarm_n].stage = stage;
    s_prewarm_recs[s_prewarm_n].id0 = id0;
    s_prewarm_recs[s_prewarm_n].id1 = id1;
    s_prewarm_recs[s_prewarm_n].key = key;
    s_prewarm_n++;
    return 1;
}

/* Rewrite the WHOLE manifest (all stages) when any stage grew. Best-effort: an
 * open/write failure is swallowed (clears dirty so we don't retry every frame).
 * File I/O only — safe to call post-device-teardown (e.g. from atexit). */
static void wgpu_prewarm_flush(void) {
    if (!wgpu_prewarm_enabled() || !s_prewarm_dirty) {
        return;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s", savedirPath(WGPU_PREWARM_FILE));
    FILE *f = mdkr_fopen_utf8(path, "w");
    if (f == NULL) {
        s_prewarm_dirty = false;   /* give up silently; no prewarm is acceptable */
        return;
    }
    for (int i = 0; i < s_prewarm_n; i++) {
        fprintf(f, "%d %016llx %08x %08x\n",
                s_prewarm_recs[i].stage,
                (unsigned long long)s_prewarm_recs[i].id0,
                (unsigned)s_prewarm_recs[i].id1,
                (unsigned)s_prewarm_recs[i].key);
    }
    fclose(f);
    s_prewarm_dirty = false;
}

/* Load the manifest into the per-stage sets once. Registers an atexit flush so a
 * single-stage native session (e.g. a headless --screenshot-exit boot) still
 * persists what it recorded even though it never hits a stage transition; web
 * relies on the transition flush + the shell's syncfs timer instead. */
static void wgpu_prewarm_ensure_loaded(void) {
    if (s_prewarm_loaded) {
        return;
    }
    s_prewarm_loaded = true;
    if (!wgpu_prewarm_enabled()) {
        return;
    }
    atexit(wgpu_prewarm_flush);
    char path[1024];
    snprintf(path, sizeof(path), "%s", savedirPath(WGPU_PREWARM_FILE));
    FILE *f = mdkr_fopen_utf8(path, "r");
    if (f == NULL) {
        return;   /* no manifest yet (first ever run) — nothing to prewarm */
    }
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        int stage; unsigned long long id0; unsigned id1, key;
        if (sscanf(line, "%d %llx %x %x", &stage, &id0, &id1, &key) == 4) {
            wgpu_prewarm_intern(stage, (uint64_t)id0, (uint32_t)id1, (uint32_t)key);
        }
    }
    fclose(f);
}

/* Record a (shader,key) actually created for the current stage. Called on every
 * genuine pipeline cache miss in wgpu_pipeline_for (before the sync/async split),
 * so it captures the exact key set that would otherwise be cold on a revisit. */
static void wgpu_prewarm_record(uint64_t id0, uint32_t id1, uint32_t key) {
    if (!wgpu_prewarm_enabled() || s_prewarm_cur_stage < 0) {
        return;
    }
    if (wgpu_prewarm_intern(s_prewarm_cur_stage, id0, id1, key)) {
        s_prewarm_dirty = true;
    }
}

#ifdef __EMSCRIPTEN__
/* PERF-005: async render-pipeline completion (web-live only; see the seam-rule
 * note at wgpuCompatCreateSurface for why this block is #ifdef'd rather than in
 * the compat header — it touches file-static cache state). Recovers prg=u1 and
 * key=u2, locates the PENDING slot the kick reserved, and installs the pipeline
 * (READY) or marks the slot FAILED. Fires only during one of the renderer-owned
 * ProcessEvents drains. The whole apparatus is compiled OUT on native, where the synchronous
 * path makes a pipeline ready the instant it is created. */
struct WgpuPipelineCallbackCtx {
    struct ShaderProgram *prg;
    uint32_t key;
    uintptr_t generation;
    uint64_t kick_host_frame;
    bool optional_shadow;
};

static void on_pipeline_ready(WGPUCreatePipelineAsyncStatus status,
                              WGPURenderPipeline pipeline,
                              WGPUStringView message,
                              void *u1, void *u2) {
    struct WgpuPipelineCallbackCtx *ctx =
        (struct WgpuPipelineCallbackCtx *)u1;
    struct ShaderProgram *prg;
    uint32_t key;
    (void)u2;
    if (ctx == NULL) {
        if (pipeline != NULL) {
            wgpuRenderPipelineRelease(pipeline);
        }
        return;
    }
    if (!s_pipeline_callback_owner_drain) {
        /* `AllowProcessEvents` promises this cannot happen. Continuing here
         * would require touching non-atomic renderer/cache state from the
         * wrong callback turn, recreating the race this contract prevents.
         * Terminate instead of pretending the branch is a thread-safe fence. */
        fprintf(stderr,
                "[webgpu] pipeline callback arrived outside renderer event "
                "drain; terminating by ownership contract\n");
        abort();
    }
    if (ctx->generation == 0 ||
        ctx->generation != s_active_work_generation) {
        /* Final shutdown or device replacement invalidated the raw program
         * pointer carried by this completion. `ctx` remains heap-owned until
         * this callback frees it, while its program is deliberately never
         * dereferenced here; a late browser completion is therefore safe even
         * after the old program cache and device session were destroyed. */
        if (pipeline != NULL) {
            wgpuRenderPipelineRelease(pipeline);
        }
        s_pipeline_callback_shutdown_late_safe++;
        free(ctx);
        return;
    }
    prg = ctx->prg;
    key = ctx->key;
    if (s_pending_pipelines > 0) {
        s_pending_pipelines--;
    }
    if (ctx->optional_shadow &&
        s_shadow_receiver_prewarm_pending > 0) {
        s_shadow_receiver_prewarm_pending--;
    }
    if (!ctx->optional_shadow) {
        /* A smoothed host opportunity can run both an authored endpoint and an
         * internal replay before yielding to the browser. Count that pair once:
         * the public pipeline budget is expressed in observable host/display
         * opportunities, not renderer passes that cannot dispatch a promise
         * between them.
         *
         * Count opportunities that were actually incomplete, not both endpoints
         * of the interval. A create kicked while host frame 570 is being drawn
         * and dispatched before frame 572's draw withheld frames 570 and 571;
         * frame 572 was complete. The old inclusive `+ 1` called that three
         * frames and could contradict maxHoldStreak for the same work. A callback
         * dispatched at the end of its kick frame still withheld that frame, so
         * the minimum remains one. */
        const uint64_t host_frame = (uint64_t)g_frameCounter;
        uint64_t pending_frames =
            host_frame > ctx->kick_host_frame
                ? host_frame - ctx->kick_host_frame
                : 1;
        if (pending_frames > UINT32_MAX) {
            pending_frames = UINT32_MAX;
        }
        if ((uint32_t)pending_frames > s_async_pipeline_frames_max) {
            s_async_pipeline_frames_max = (uint32_t)pending_frames;
            if (getenv("MDKR_WEBGPU_PIPELINE_TRACE") != NULL) {
                fprintf(
                    stderr,
                    "[WGPU-PIPELINE] shader=%016llx/%08x key=0x%03x "
                    "kick=%llu ready=%llu incomplete=%u\n",
                    (unsigned long long)prg->shader_id0,
                    (unsigned)prg->shader_id1,
                    (unsigned)key,
                    (unsigned long long)ctx->kick_host_frame,
                    (unsigned long long)host_frame,
                    (unsigned)s_async_pipeline_frames_max);
            }
        }
    }
    /* Find the PENDING slot this create was kicked for. At most one PENDING entry
     * per key exists: the kick reserves it, and every later draw with the same key
     * finds it in the lookup and returns NULL without re-kicking. */
    struct WgpuPipeEntry *e = NULL;
    for (int i = 0; i < prg->npipes; i++) {
        if (prg->pipes[i].key == key && prg->pipes[i].state == WGPU_PIPE_PENDING) {
            e = &prg->pipes[i];
            break;
        }
    }
    if (e == NULL) {
        /* Reservation never evicts PENDING entries. Missing one therefore means
         * cache corruption; release the orphan and stop at the scheduler
         * boundary rather than continuing with an unexplained missing draw. */
        if (pipeline != NULL) {
            wgpuRenderPipelineRelease(pipeline);
        }
        fprintf(stderr,
                "[webgpu] async pipeline completion lost its reserved slot "
                "(key=0x%03x)\n", (unsigned)key);
        s_pipeline_failures++;
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics pipeline cache became inconsistent. Reload the page "
            "to continue from the last persisted save.");
        free(ctx);
        return;
    }
    if (status == WGPUCreatePipelineAsyncStatus_Success && pipeline != NULL) {
        e->pipe = pipeline;
        e->state = WGPU_PIPE_READY;
        s_async_pipeline_ready++;
    } else {
        e->pipe = NULL;
        e->state = WGPU_PIPE_FAILED;
        s_async_pipeline_failed++;
        if (ctx->optional_shadow) {
            /*
             * Receiver pipelines are optional enhancement resources. Keep the
             * ordinary world pipelines and projected decals rather than
             * converting a background-prewarm failure into missing geometry or
             * a browser-fatal panel.
             */
            s_shadow_receiver_prewarm_failed = true;
            s_shadow_resource_perma_fail = true;
            s_shadow_resource_failures++;
            fprintf(stderr,
                    "[world-shadow] optional receiver pipeline failed "
                    "(key=0x%03x status=%d); projected shadows remain active: "
                    "%.*s\n",
                    (unsigned)key, (int)status,
                    (int)message.length,
                    message.data ? message.data : "");
            free(ctx);
            return;
        }
        /* A permanently failed required pipeline is not a degraded visual mode:
         * it means geometry would be absent for the remainder of the session. */
        fprintf(stderr,
                "[webgpu] async pipeline create failed "
                "(key=0x%03x status=%d): %.*s\n",
                (unsigned)key, (int)status,
                (int)message.length, message.data ? message.data : "");
        s_pipeline_failures++;
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend could not compile a required rendering "
            "pipeline. Reload the page to continue from the last persisted "
            "save.");
    }
    free(ctx);
}

static bool wgpu_kick_async_pipeline(
    struct ShaderProgram *prg,
    uint32_t key,
    const WGPURenderPipelineDescriptor *pd,
    bool optional_shadow) {
    int slot = wgpu_pipe_reserve_slot(prg);
    if (slot < 0) {
        return false;
    }
    prg->pipes[slot].key = key;
    prg->pipes[slot].pipe = NULL;
    prg->pipes[slot].state = WGPU_PIPE_PENDING;
    struct WgpuPipelineCallbackCtx *ctx =
        (struct WgpuPipelineCallbackCtx *)malloc(sizeof(*ctx));
    if (ctx == NULL) {
        prg->pipes[slot].state = WGPU_PIPE_FAILED;
        return false;
    }
    ctx->prg = prg;
    ctx->key = key;
    ctx->generation = s_active_work_generation;
    ctx->kick_host_frame = (uint64_t)g_frameCounter;
    ctx->optional_shadow = optional_shadow;
    s_pending_pipelines++;
    if (optional_shadow) {
        s_shadow_receiver_prewarm_pending++;
    }
    s_async_pipeline_creates++;
    if ((uint32_t)s_pending_pipelines > s_async_pending_high_water) {
        s_async_pending_high_water = (uint32_t)s_pending_pipelines;
    }
    WGPUCreateRenderPipelineAsyncCallbackInfo cb = {0};
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = on_pipeline_ready;
    cb.userdata1 = ctx;
    cb.userdata2 = NULL;
    if (!optional_shadow &&
        gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_PIPELINE_ASYNC)) {
        const bool previous = s_pipeline_callback_owner_drain;
        s_pipeline_callback_owner_drain = true;
        on_pipeline_ready(
            WGPUCreatePipelineAsyncStatus_ValidationError, NULL,
            wgpu_sv("deterministic MDKR_WEBGPU_FAULT injection"),
            ctx, NULL);
        s_pipeline_callback_owner_drain = previous;
    } else {
        wgpuDeviceCreateRenderPipelineAsync(s_device, pd, cb);
    }
    return true;
}
#endif  /* __EMSCRIPTEN__ */

/* Lazily build + cache the render pipeline for the current (shader, blend, depth)
 * dynamic state — WebGPU bakes all of it into the pipeline (mtl_pso_for shape).
 *
 * PERF-005: the descriptor half is factored into wgpu_fill_pipeline_desc so the
 * sync and async create paths build a bit-identical descriptor. On web-live
 * (non-deterministic) a cache miss kicks an async create and returns NULL —
 * wgpu_draw_triangles skips the batch this frame (transient pop-in) until the
 * pipeline lands, eliminating the first-sight synchronous compile hitch. Native,
 * and web under --deterministic, keep the synchronous create so every byte-exact
 * gate (parity/screenshot/tape, all --deterministic) stays on the identical HEAD
 * path. */
static WGPURenderPipeline wgpu_pipeline_for(struct ShaderProgram *prg, enum GfxBlendMode blend) {
    if (prg->module == NULL) {
        return NULL;
    }
    bool decal = wgpu_depth_is_decal();
    uint32_t key = (uint32_t)blend
                 | ((uint32_t)(s_depth_test ? 1 : 0)    << 4)
                 | ((uint32_t)(s_depth_update ? 1 : 0)  << 5)
                 | ((uint32_t)(s_depth_compare ? 1 : 0) << 6)
                 | ((uint32_t)(decal ? 1 : 0)           << 7)
                 | ((uint32_t)(wgpu_preserve_cov_alpha(blend) ? 1 : 0) << 8);
    for (int i = 0; i < prg->npipes; i++) {
        if (prg->pipes[i].key == key) {
            struct WgpuPipeEntry *e = &prg->pipes[i];
            if (e->state == WGPU_PIPE_READY) {
                return e->pipe;
            }
            /* PENDING (async create in flight) or FAILED: skip the batch this
             * frame. Returning here — rather than falling through to the miss
             * path — is what prevents re-kicking a create for a key already in
             * flight (or one that failed). Native never stores a non-READY entry,
             * so it never reaches this and its behavior is unchanged from HEAD.
             * PERF-005b: a PENDING skip marks the frame visually incomplete so
             * wgpu_end_frame withholds its present; FAILED does not (permanent). */
            if (e->state == WGPU_PIPE_PENDING) {
                s_frame_pending_skips++;
            }
            return NULL;
        }
    }

    /* Cache miss. PERF-005 Phase 2: record this (shader,key) for the current stage
     * so a future load screen can prewarm it (both the sync and async create below
     * are genuine first-sight CREATEs — this is the one place to capture them). */
    wgpu_prewarm_record(prg->shader_id0, prg->shader_id1, key);

    /* Build the descriptor ONCE via the shared helper; both the sync and async
     * paths below submit this exact descriptor. */
    struct WgpuPipeScratch sc;
    WGPURenderPipelineDescriptor pd;
    wgpu_fill_pipeline_desc(prg, key, &pd, &sc);

#ifdef __EMSCRIPTEN__
    /* Web-live async path — gated OFF under --deterministic so every recorded
     * gate stays on the synchronous path. Native never compiles this block (the
     * async create/callback API has no native test coverage), so native is always
     * synchronous and byte-for-byte HEAD. Reserve a PENDING slot FIRST so
     * subsequent draws for this key hit the lookup above and do NOT re-kick, then
     * fire the async create and skip this batch this frame. */
    extern int g_deterministic;
    if (!g_deterministic) {
        if (!wgpu_kick_async_pipeline(prg, key, &pd, false)) {
            fprintf(stderr,
                    "[webgpu] could not start required async pipeline for "
                    "shader %016llx/%08x (cache pressure or allocation "
                    "failure)\n",
                    (unsigned long long)prg->shader_id0,
                    (unsigned)prg->shader_id1);
            s_pipeline_failures++;
            s_ready = false;
            s_runtime_status = GFX_RENDERING_FATAL;
            WGPU_COMPAT_REPORT_FAILURE(
                "The graphics backend exhausted its pipeline compilation "
                "budget. Reload the page to continue from the last persisted "
                "save.");
            return NULL;
        }
        s_frame_pending_skips++;   /* PERF-005b: this batch is missing from the frame */
        return NULL;
    }
#endif

    /* Synchronous create: native always; web under --deterministic. Identical to
     * the pre-PERF-005 behavior (a failed create stores nothing and is retried on
     * the next draw), now stamping the entry's state READY. */
    WGPURenderPipeline pipe = WGPU_FAULT_CREATE(
        PIPELINE_SYNC, wgpuDeviceCreateRenderPipeline(s_device, &pd));
    if (pipe != NULL) {
        int slot = wgpu_pipe_reserve_slot(prg);
        if (slot < 0) {
            wgpuRenderPipelineRelease(pipe);
            fprintf(stderr,
                    "[webgpu] no completed pipeline slot is available for "
                    "shader %016llx/%08x\n",
                    (unsigned long long)prg->shader_id0,
                    (unsigned)prg->shader_id1);
            s_pipeline_failures++;
            s_ready = false;
            s_runtime_status = GFX_RENDERING_FATAL;
            WGPU_COMPAT_REPORT_FAILURE(
                "The graphics backend exhausted its pipeline cache. Reload "
                "the page to continue from the last persisted save.");
            return NULL;
        }
        prg->pipes[slot].key = key;
        prg->pipes[slot].pipe = pipe;
        prg->pipes[slot].state = WGPU_PIPE_READY;
    } else {
        fprintf(stderr,
                "[webgpu] render-pipeline creation failed "
                "(shader=%016llx/%08x key=0x%03x)\n",
                (unsigned long long)prg->shader_id0,
                (unsigned)prg->shader_id1, (unsigned)key);
        s_pipeline_failures++;
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        WGPU_COMPAT_REPORT_FAILURE(
            "The graphics backend could not compile a required rendering "
            "pipeline. Reload the page to continue from the last persisted "
            "save.");
    }
    return pipe;
}

/*
 * World-shadow receiver shaders are substantially heavier than the ordinary
 * N64 combiners on browser drivers. Compile their exact pipeline twins in the
 * background while the map-validity gate deliberately retains the ordinary
 * shader plus projected decals. This makes the optional enhancement atomic:
 * no missing-geometry frame, no presentation hold, and no shadow/decal gap.
 */
static bool wgpu_shadow_prewarm_receivers(void) {
#ifndef __EMSCRIPTEN__
    return true;
#else
    extern int g_deterministic;
    if (g_deterministic) {
        return true;
    }
    if (s_shadow_receiver_prewarm_failed ||
        s_shadow_resource_perma_fail) {
        return false;
    }

    const size_t shader_count = s_shader_count;
    size_t pipe_count = 0;
    for (size_t index = 0; index < shader_count; index++) {
        if (s_shaders[index] != NULL) {
            pipe_count += (size_t)s_shaders[index]->npipes;
        }
    }
    if (s_shadow_prewarm_memo_valid &&
        s_shadow_prewarm_memo_shaders == shader_count &&
        s_shadow_prewarm_memo_pipes == pipe_count &&
        s_shadow_prewarm_memo_pending == s_shadow_receiver_prewarm_pending &&
        s_shadow_prewarm_memo_inflight == s_pending_pipelines) {
        return s_shadow_prewarm_memo_result;
    }

    struct ShaderProgram *saved_shader = s_cur_shader;
    bool has_receiver_candidate = false;
    for (size_t index = 0; index < shader_count; index++) {
        struct ShaderProgram *base = s_shaders[index];
        struct ShaderProgram *receiver = NULL;
        if (base == NULL ||
            (base->shader_id1 & SHADER_OPT_SUN_SHADOW) != 0) {
            continue;
        }
        for (int pipe_index = 0;
             pipe_index < base->npipes;
             pipe_index++) {
            const struct WgpuPipeEntry *base_pipe =
                &base->pipes[pipe_index];
            const uint32_t key = base_pipe->key;
            const uint32_t blend = key & 0x0fu;
            const bool depth_test = (key & (1u << 4)) != 0;
            const bool depth_compare = (key & (1u << 6)) != 0;
            const bool decal = (key & (1u << 7)) != 0;
            const bool cutout =
                (base->shader_id1 & SHADER_OPT_TEXTURE_EDGE) != 0;

            if (base_pipe->state != WGPU_PIPE_READY ||
                !depth_test || !depth_compare || decal ||
                (blend != (uint32_t)GFX_BLEND_DISABLED && !cutout)) {
                continue;
            }
            if (receiver == NULL) {
                const uint32_t receiver_id1 =
                    base->shader_id1 |
                    SHADER_OPT_WORLD_POS |
                    SHADER_OPT_SUN_SHADOW;
                receiver = wgpu_lookup_shader(
                    base->shader_id0, receiver_id1);
                if (receiver == NULL) {
                    receiver = wgpu_create_and_load_new_shader(
                        base->shader_id0, receiver_id1);
                }
                if (receiver == NULL || receiver->module == NULL) {
                    s_shadow_receiver_prewarm_failed = true;
                    s_shadow_resource_perma_fail = true;
                    s_shadow_resource_failures++;
                    fprintf(stderr,
                            "[world-shadow] could not create an optional "
                            "receiver shader; projected shadows remain active\n");
                    s_cur_shader = saved_shader;
                    return false;
                }
            }

            bool found = false;
            for (int receiver_pipe = 0;
                 receiver_pipe < receiver->npipes;
                 receiver_pipe++) {
                if (receiver->pipes[receiver_pipe].key != key) {
                    continue;
                }
                found = true;
                if (receiver->pipes[receiver_pipe].state ==
                    WGPU_PIPE_FAILED) {
                    s_shadow_receiver_prewarm_failed = true;
                    s_shadow_resource_perma_fail = true;
                    s_shadow_resource_failures++;
                    fprintf(stderr,
                            "[world-shadow] optional receiver pipeline is "
                            "unavailable; projected shadows remain active\n");
                }
                break;
            }
            if (found) {
                has_receiver_candidate = true;
                continue;
            }

            struct WgpuPipeScratch scratch;
            WGPURenderPipelineDescriptor descriptor;
            wgpu_fill_pipeline_desc(
                receiver, key, &descriptor, &scratch);
            if (!wgpu_kick_async_pipeline(
                    receiver, key, &descriptor, true)) {
                s_shadow_receiver_prewarm_failed = true;
                s_shadow_resource_perma_fail = true;
                s_shadow_resource_failures++;
                fprintf(stderr,
                        "[world-shadow] could not start optional receiver "
                        "pipeline prewarm; projected shadows remain active\n");
                s_cur_shader = saved_shader;
                return false;
            }
            has_receiver_candidate = true;
        }
    }
    s_cur_shader = saved_shader;
    {
        const bool verdict = has_receiver_candidate &&
                             s_shadow_receiver_prewarm_pending == 0 &&
                             !s_shadow_receiver_prewarm_failed;
        /* Stamp the key the scan actually finished against: it may have created
         * twins, so re-read the counts rather than reusing the ones sampled on
         * entry. A frame that did work therefore rescans next frame, and only a
         * completely unchanged program cache short-circuits. */
        s_shadow_prewarm_memo_shaders = s_shader_count;
        s_shadow_prewarm_memo_pipes = 0;
        for (size_t index = 0; index < s_shader_count; index++) {
            if (s_shaders[index] != NULL) {
                s_shadow_prewarm_memo_pipes +=
                    (size_t)s_shaders[index]->npipes;
            }
        }
        s_shadow_prewarm_memo_pending = s_shadow_receiver_prewarm_pending;
        s_shadow_prewarm_memo_inflight = s_pending_pipelines;
        s_shadow_prewarm_memo_result = verdict;
        s_shadow_prewarm_memo_valid = true;
        return verdict;
    }
#endif
}

/* PERF-005 Phase 2: switch the recorder to `stage`. Flushes the PREVIOUS stage's
 * manifest first if it grew during play (persist at the transition, not per-create).
 * Called from boss.c on every stage (re)load, adjacent to gfx_webgpu_prewarm_stage.
 * A near no-op when prewarm is disabled (deterministic / GE007_PIPECACHE=0) or when
 * the webgpu backend isn't the active one (nothing ever records, so nothing flushes).
 * Declared in gfx_webgpu.h. */
void gfx_webgpu_set_stage(int stage) {
    if (!wgpu_prewarm_enabled()) {
        s_prewarm_cur_stage = stage;   /* harmless; record() is a no-op when disabled */
        return;
    }
    wgpu_prewarm_ensure_loaded();
    if (s_prewarm_dirty) {
        wgpu_prewarm_flush();          /* persist the stage we are leaving */
    }
    s_prewarm_cur_stage = stage;
}

/* PERF-005 Phase 2: synchronously build every pipeline recorded for `stage` on a
 * prior visit/session, so its materials are already READY before the first draw —
 * no cold-material pop-in / present-hold on entry. Runs inside boss.c's load window
 * (watchdog suppressed, load screen up); the creates are the POINT of that window.
 *
 * IMPORTANT (Asyncify, PERF-031): this must NOT suspend. wgpuDeviceCreateRenderPipeline
 * (sync) and the shader-module create it may trigger are non-suspending on both
 * dialects — the only suspending calls are adapter/device bring-up pumps, which are
 * absent here. Do NOT add emscripten_sleep / ProcessEvents in this path.
 *
 * Bit-identical guarantee: the descriptor is built through the SHARED
 * wgpu_fill_pipeline_desc, so a prewarmed pipeline is byte-for-byte what the draw
 * would have created for the same key — no async/sync render divergence.
 * Declared in gfx_webgpu.h. */
void gfx_webgpu_prewarm_stage(int stage) {
    if (!wgpu_prewarm_enabled()) {
        return;
    }
    if (!s_ready || s_device == NULL) {
        return;   /* device not up yet — records still accrue; warm on the next visit */
    }
    wgpu_prewarm_ensure_loaded();

    /* wgpu_create_and_load_new_shader sets s_cur_shader as a side effect; prewarm
     * must not perturb the draw path's current-shader state, so snapshot + restore. */
    struct ShaderProgram *saved_cur = s_cur_shader;
    int considered = 0, warmed = 0, shaders_built = 0;

    for (int i = 0; i < s_prewarm_n; i++) {
        if (s_prewarm_recs[i].stage != stage) {
            continue;
        }
        considered++;
        uint64_t id0 = s_prewarm_recs[i].id0;
        uint32_t id1 = s_prewarm_recs[i].id1;
        uint32_t key = s_prewarm_recs[i].key;

        struct ShaderProgram *prg = wgpu_lookup_shader(id0, id1);
        if (prg == NULL) {
            /* Shader not created yet (its first draw hasn't happened). Build it now
             * — pure function of the ids (WGSL derived from the combiner encoding),
             * no gfx_pc render state needed. This is the crux that makes second-launch
             * cold-warming possible. */
            prg = wgpu_create_and_load_new_shader(id0, id1);
            if (prg != NULL && prg->module != NULL) {
                shaders_built++;
            }
        }
        if (prg == NULL || prg->module == NULL) {
            continue;   /* inert program (WGSL build failed) — skip, draw path guards too */
        }

        /* Already READY (prewarmed earlier this session, or created by a draw)? skip. */
        bool have = false;
        for (int j = 0; j < prg->npipes; j++) {
            if (prg->pipes[j].key == key && prg->pipes[j].state == WGPU_PIPE_READY) {
                have = true;
                break;
            }
        }
        if (have) {
            continue;
        }

        struct WgpuPipeScratch sc;
        WGPURenderPipelineDescriptor pd;
        wgpu_fill_pipeline_desc(prg, key, &pd, &sc);
        WGPURenderPipeline pipe = WGPU_FAULT_CREATE(
            PIPELINE_PREWARM,
            wgpuDeviceCreateRenderPipeline(s_device, &pd));   /* SYNC */
        if (pipe != NULL) {
            int slot = wgpu_pipe_reserve_slot(prg);
            if (slot < 0) {
                wgpuRenderPipelineRelease(pipe);
                fprintf(stderr,
                        "[webgpu] prewarm exhausted pipeline slots for "
                        "shader %016llx/%08x\n",
                        (unsigned long long)prg->shader_id0,
                        (unsigned)prg->shader_id1);
                s_ready = false;
                s_runtime_status = GFX_RENDERING_FATAL;
                break;
            }
            prg->pipes[slot].key = key;
            prg->pipes[slot].pipe = pipe;
            prg->pipes[slot].state = WGPU_PIPE_READY;
            warmed++;
        } else {
            fprintf(stderr,
                    "[webgpu] prewarm pipeline creation failed "
                    "(shader=%016llx/%08x key=0x%03x)\n",
                    (unsigned long long)prg->shader_id0,
                    (unsigned)prg->shader_id1, (unsigned)key);
            s_ready = false;
            s_runtime_status = GFX_RENDERING_FATAL;
            break;
        }
    }

    s_cur_shader = saved_cur;

    if (port_env_bool("GE007_PIPECACHE_TRACE", 0,
            "PERF-005 Phase 2: log record/replay pipeline prewarm counts at level load")) {
        fprintf(stderr, "[webgpu] pipecache prewarm stage %d: warmed %d pipeline(s), "
                        "built %d shader(s), %d recorded\n",
                stage, warmed, shaders_built, considered);
        fflush(stderr);
    }
}

/* ------------------------------------------------------------------------
 * Vtable: textures + samplers
 *
 * The N64 interpreter's model (mirroring gfx_opengl.c): new_texture() hands out
 * an opaque id; select_texture(tile, id) makes that id current on a tile;
 * upload_texture(rgba,w,h) uploads into the id current on the most-recently
 * selected tile; set_sampler_parameters(tile,...) sets the filter/wrap for that
 * tile. WebGPU has no global bind state, so textures + samplers are looked up
 * by id and staged per-tile here; the draw path binds tile 0/1's view
 * + sampler into a bind group at submit time.
 *
 * Ids are recycled through a free-list over a growable array (like glGenTextures
 * reuse), so live GPU resources stay bounded by the interpreter's texture cache
 * (~1024) rather than growing with monotonic ids over a long session.
 * ---------------------------------------------------------------------- */
#define WGPU_TX_MIRROR 0x1u   /* G_TX_MIRROR (PR/gbi.h) — mirrored here to avoid
                                 pulling the N64 GBI headers into this TU */
#define WGPU_TX_CLAMP  0x2u   /* G_TX_CLAMP */

/* PERF-019: per-texture reverse index into the bind-group cache. Each entry records
 * the bg-cache slot indices whose key references THIS texture's current `view` (in
 * key slot 1 or 3). Releasing the view then walks only these candidate slots instead
 * of sweeping all WGPU_BG_CACHE entries—the level-transition delete storm
 * (gfx_clear_texture_cache deletes every pooled texture) drops from
 * texture_count×cache-size
 * to texture_count×refs. The list is a conservative SUPERSET: a slot round-robin-
 * evicted+reused after registration stays listed (a stale candidate) but is filtered
 * at release by re-checking the exact same pointer predicate the full sweep uses, so
 * the invalidation DECISION is byte-identical (WEB-068 ABA discipline preserved). On
 * overflow (a texture referenced by more than WGPU_TEX_BG_REFS distinct slots) the
 * entry falls back to the exact full sweep — always correct, just not accelerated. */
#define WGPU_TEX_BG_REFS 64
struct WgpuTexEntry {
    WGPUTexture     tex;
    WGPUTextureView view;
    int             w, h;
    int             levels;                         /* mdkr64: >1 when a CPU mip chain was uploaded */
    bool            used;
    bool            bg_ref_overflow;                /* list overflowed → release full-sweeps */
    uint8_t         bg_ref_n;                       /* live candidate count */
    uint16_t        bg_refs[WGPU_TEX_BG_REFS];      /* bg-cache slot indices (0..WGPU_BG_CACHE-1) */
    uint32_t        draw_epoch;                     /* WEB-053: encoder epoch of the last recorded draw */
};
static struct WgpuTexEntry *s_tex = NULL;   /* indexed by (id - 1) */
static uint32_t s_tex_cap = 0;
static uint32_t s_tex_hi = 0;               /* highest id ever allocated */
static uint32_t s_tex_high_water = 0;
static uint32_t *s_tex_free = NULL;         /* stack of freed ids for reuse */
static uint32_t s_tex_free_n = 0, s_tex_free_cap = 0;

/* PERF-019: register a bg-cache slot as referencing this texture's view. Called from
 * the draw path when a bind group is (re)built. Deduped so a slot re-inserted for the
 * same view after an eviction is not double-listed; on a full list the entry flips to
 * overflow (→ full sweep on release) rather than dropping a reference (which would
 * under-invalidate — the ABA correctness bug). NULL entry = the white fallback view,
 * which is never released, so it needs no reverse index. */
static void wgpu_tex_bg_ref_add(struct WgpuTexEntry *e, uint32_t slot) {
    if (e == NULL || e->bg_ref_overflow) {
        return;
    }
    for (uint8_t i = 0; i < e->bg_ref_n; i++) {
        if (e->bg_refs[i] == (uint16_t)slot) {
            return;   /* already listed */
        }
    }
    if (e->bg_ref_n >= WGPU_TEX_BG_REFS) {
        e->bg_ref_overflow = true;
        return;
    }
    e->bg_refs[e->bg_ref_n++] = (uint16_t)slot;
}

/* PERF-019: invalidate every cached bind group that references this texture's view,
 * using the reverse index instead of a full 512-entry sweep. Must be called BEFORE
 * the view's C-handle is released (reads e->view). Semantics are identical to
 * wgpu_bg_cache_invalidate_view(e->view): the per-slot drop predicate is byte-for-byte
 * the same, so no referencing entry survives (WEB-068). Clears the list afterward —
 * the old view is gone; a recreated texture starts with an empty index. */
static void wgpu_bg_cache_invalidate_view_indexed(struct WgpuTexEntry *e) {
    if (e == NULL || e->view == NULL) {
        return;
    }
    if (e->bg_ref_overflow) {
        wgpu_bg_cache_invalidate_view(e->view);   /* list overflowed — exact full sweep */
    } else {
        const void *v = (const void *)e->view;
        for (uint8_t i = 0; i < e->bg_ref_n; i++) {
            struct WgpuBgEntry *slot = &s_bg_cache_tab[e->bg_refs[i]];
            /* Re-verify against the SAME predicate the full sweep uses: a slot may have
             * been evicted+reused since registration, so drop it only if it still
             * references v (keeps the decision identical to the full sweep). */
            if (slot->bg != NULL &&
                (slot->key[1] == v || slot->key[3] == v ||
                 slot->key[5] == v || slot->key[7] == v)) {
                wgpu_release_cached_bind_group(slot->bg);
                slot->bg = NULL;
                memset(slot->key, 0, sizeof(slot->key));
                if (s_bg_cache_live > 0) {
                    s_bg_cache_live--;
                }
            }
        }
    }
    e->bg_ref_n = 0;
    e->bg_ref_overflow = false;
}

/* Per-tile binding staged by select_texture / set_sampler_parameters and read by
 * the draw path. */
static uint32_t    s_bound_tex[2] = {0, 0};
static WGPUSampler s_bound_sampler[2] = {NULL, NULL};
static int         s_active_tile = 0;       /* tile of the last select_texture */

/* Sampler cache keyed by (linear, cms, cmt). Small: the N64 uses a handful of
 * distinct (filter, wrap) combinations. */
struct WgpuSamplerEntry { int linear; int mips; uint32_t cms, cmt; WGPUSampler sampler; };
static struct WgpuSamplerEntry s_samplers[64];
static int s_sampler_n = 0;
static int s_sampler_high_water = 0;

static uint32_t wgpu_cm_key(uint32_t cm) {
    if ((cm & WGPU_TX_CLAMP) != 0) {
        return WGPU_TX_CLAMP;
    }
    return (cm & WGPU_TX_MIRROR) != 0 ? WGPU_TX_MIRROR : 0;
}

static WGPUAddressMode wgpu_cm_to_address(uint32_t cm) {
    if (cm & WGPU_TX_CLAMP) {
        return WGPUAddressMode_ClampToEdge;
    }
    return (cm & WGPU_TX_MIRROR) ? WGPUAddressMode_MirrorRepeat : WGPUAddressMode_Repeat;
}

static struct WgpuTexEntry *wgpu_tex_lookup(uint32_t id) {
    if (id == 0 || id > s_tex_hi) {
        return NULL;
    }
    struct WgpuTexEntry *e = &s_tex[id - 1];
    return e->used ? e : NULL;
}

static uint32_t wgpu_new_texture(void) {
    uint32_t id;
    if (s_tex_free_n > 0) {
        id = s_tex_free[--s_tex_free_n];
    } else {
        id = ++s_tex_hi;
        if (id > s_tex_cap) {
            uint32_t ncap = s_tex_cap ? s_tex_cap * 2 : 256;
            struct WgpuTexEntry *n = (struct WgpuTexEntry *)realloc(s_tex, ncap * sizeof(*s_tex));
            if (n == NULL) {
                --s_tex_hi;
                return 0;   /* allocation failure — interpreter treats 0 as none */
            }
            memset(n + s_tex_cap, 0, (ncap - s_tex_cap) * sizeof(*s_tex));
            s_tex = n;
            s_tex_cap = ncap;
        }
    }
    struct WgpuTexEntry *e = &s_tex[id - 1];
    e->tex = NULL;
    e->view = NULL;
    e->w = e->h = 0;
    e->used = true;
    e->bg_ref_n = 0;             /* PERF-019: fresh id owns no cached bind groups yet */
    e->bg_ref_overflow = false;
    e->draw_epoch = 0;           /* WEB-053: a fresh id has never been drawn */
    e->levels = 0;               /* ids recycle LIFO, so a fresh id must not inherit the
                                  * previous tenant's mip count: set_sampler_parameters()
                                  * and the upload predicate both read it as this
                                  * texture's own state. */
    if (id > s_tex_high_water) {
        s_tex_high_water = id;
    }
    return id;
}

static void wgpu_delete_texture(uint32_t texture_id) {
    struct WgpuTexEntry *e = wgpu_tex_lookup(texture_id);
    if (e == NULL) {
        return;
    }
    if (e->view != NULL) {
        wgpu_bg_cache_invalidate_view_indexed(e);   /* PERF-019/WEB-068: purge stale cache refs (reverse index) */
        wgpuTextureViewRelease(e->view); e->view = NULL;
    }
    if (e->tex != NULL)  { wgpuTextureRelease(e->tex);      e->tex = NULL; }
    e->used = false;
    if (s_tex_free_n >= s_tex_free_cap) {
        uint32_t ncap = s_tex_free_cap ? s_tex_free_cap * 2 : 256;
        uint32_t *n = (uint32_t *)realloc(s_tex_free, ncap * sizeof(*n));
        if (n == NULL) {
            return;   /* drop the id (never reused); GPU resource already freed */
        }
        s_tex_free = n;
        s_tex_free_cap = ncap;
    }
    s_tex_free[s_tex_free_n++] = texture_id;
}

static void wgpu_select_texture(int tile, uint32_t texture_id) {
    if (tile < 0 || tile > 1) {
        return;
    }
    s_active_tile = tile;
    s_bound_tex[tile] = texture_id;
}

static bool wgpu_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    if (!s_ready || rgba32_buf == NULL || width <= 0 || height <= 0 ||
        (uint32_t)width > s_max_tex_dim || (uint32_t)height > s_max_tex_dim) {
        return false;
    }
    struct WgpuTexEntry *e = wgpu_tex_lookup(s_bound_tex[s_active_tile]);
    if (e == NULL) {
        return false;
    }

    /* The copy descriptor is identical for the in-place and recreate paths. */
    WGPUTexelCopyTextureInfo dst = {0};
    dst.mipLevel = 0;
    dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout = {0};
    layout.offset = 0;
    layout.bytesPerRow = (uint32_t)width * 4u;
    layout.rowsPerImage = (uint32_t)height;
    WGPUExtent3D ext = { (uint32_t)width, (uint32_t)height, 1 };
    const size_t bytes = (size_t)width * (size_t)height * 4u;

    /* WEB-053: a re-upload whose dimensions match the live texture (the format
     * is always RGBA8Unorm here) writes into the existing texture in place
     * instead of destroy+recreate. Keeps the WGPUTexture AND its view — so any
     * cached draw bind groups that reference the view stay valid and warm —
     * eliminating per-frame texture churn for animated/streamed surfaces.
     * ORDERING INVARIANT: wgpuQueueWriteTexture executes
     * BEFORE the frame's command buffer, so a same-id re-upload issued after
     * an earlier draw in the SAME frame would retroactively swap that draw's
     * texels (the old destroy+recreate path pinned the old texture via the
     * bind group and was safe by construction).
     *
     * That hazard IS reachable: the frontend's texture cache is a round-robin
     * ring whose cursor advances on every miss, so once the ring has wrapped it
     * replaces still-live entries by re-uploading into the SAME backend id
     * (platform/fast3d/gfx_pc_dkr.c, dkr_bind_texture). Under cache pressure the
     * replaced entry can be one this frame has already drawn. The ordering
     * invariant is therefore enforced rather than assumed: a texture stamped
     * with the live encoder epoch is read by a recorded, unsubmitted draw, so it
     * takes the recreate path below. The old WGPUTexture stays alive through the
     * bind group that references it until that command buffer is submitted. */
    if (e->tex != NULL && e->view != NULL && e->w == width && e->h == height &&
        e->levels <= 1 && e->draw_epoch != s_draw_epoch) {
        dst.texture = e->tex;
        wgpuQueueWriteTexture(s_queue, &dst, rgba32_buf, bytes, &layout, &ext);
        return true;
    }

    /* Dimensions changed (or first upload into this id): build the replacement
     * completely before releasing the live texture/view. Allocation failure
     * therefore leaves the prior material byte-for-byte usable. */
    WGPUTextureDescriptor td = {0};
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)width;
    td.size.height = (uint32_t)height;
    td.size.depthOrArrayLayers = 1;
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    WGPUTexture new_tex = WGPU_FAULT_CREATE(
        TEXTURE, wgpuDeviceCreateTexture(s_device, &td));
    if (new_tex == NULL) {
        fprintf(stderr, "[webgpu] texture allocation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not allocate a required texture. "
            "Reload the page to continue from the last persisted save.");
        return false;
    }
    dst.texture = new_tex;
    wgpuQueueWriteTexture(s_queue, &dst, rgba32_buf, bytes, &layout, &ext);

    WGPUTextureView new_view = WGPU_FAULT_CREATE(
        TEXTURE_VIEW, wgpuTextureCreateView(new_tex, NULL));
    if (new_view == NULL) {
        wgpuTextureRelease(new_tex);
        fprintf(stderr, "[webgpu] texture-view allocation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not allocate a required texture view. "
            "Reload the page to continue from the last persisted save.");
        return false;
    }
    if (e->view != NULL) {
        wgpu_bg_cache_invalidate_view_indexed(e);
        wgpuTextureViewRelease(e->view);
    }
    if (e->tex != NULL) {
        wgpuTextureRelease(e->tex);
    }
    e->tex = new_tex;
    e->view = new_view;
    e->w = width;
    e->h = height;
    e->levels = 1;
    /* WEB-053: the replacement objects carry no recorded reads. Clearing the
     * stamp lets a further same-frame re-upload of this id take the cheap
     * in-place path again, right up until the next draw stamps it. */
    e->draw_epoch = 0;
    return true;
}

/*
 * mdkr64: upload a CPU-built mip chain. The levels come from
 * platform/fast3d/gfx_mipgen.c, so no driver-side generation is involved and
 * the NPOT hazard that disabled mipmaps in this port cannot apply.
 */
static bool wgpu_upload_texture_mipped(const uint8_t *const *level_rgba,
                                       const int *level_w, const int *level_h,
                                       int level_count) {
    if (!s_ready || level_rgba == NULL || level_w == NULL || level_h == NULL ||
        level_count <= 0 || level_count > 16) {
        return false;
    }
    struct WgpuTexEntry *e = wgpu_tex_lookup(s_bound_tex[s_active_tile]);
    if (e == NULL) {
        return false;
    }
    if ((uint32_t)level_w[0] > s_max_tex_dim || (uint32_t)level_h[0] > s_max_tex_dim) {
        return false;
    }

    /* Always recreate: mipLevelCount is fixed at creation. Build into
     * temporaries so any failure preserves the prior live material. */
    WGPUTexture new_tex;
    {
        WGPUTextureDescriptor td = {0};
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        td.dimension = WGPUTextureDimension_2D;
        td.size.width = (uint32_t)level_w[0];
        td.size.height = (uint32_t)level_h[0];
        td.size.depthOrArrayLayers = 1;
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.mipLevelCount = (uint32_t)level_count;
        td.sampleCount = 1;
        new_tex = WGPU_FAULT_CREATE(
            MIP_TEXTURE, wgpuDeviceCreateTexture(s_device, &td));
    }
    if (new_tex == NULL) {
        return false;
    }

    for (int l = 0; l < level_count; l++) {
        WGPUTexelCopyTextureInfo dst = {0};
        WGPUTexelCopyBufferLayout layout = {0};
        WGPUExtent3D ext = { (uint32_t)level_w[l], (uint32_t)level_h[l], 1 };
        dst.texture = new_tex;
        dst.mipLevel = (uint32_t)l;
        dst.aspect = WGPUTextureAspect_All;
        layout.offset = 0;
        layout.bytesPerRow = (uint32_t)level_w[l] * 4u;
        layout.rowsPerImage = (uint32_t)level_h[l];
        wgpuQueueWriteTexture(s_queue, &dst, level_rgba[l],
                              (size_t)level_w[l] * (size_t)level_h[l] * 4u,
                              &layout, &ext);
    }

    WGPUTextureView new_view = WGPU_FAULT_CREATE(
        MIP_VIEW, wgpuTextureCreateView(new_tex, NULL));
    if (new_view == NULL) {
        wgpuTextureRelease(new_tex);
        return false;
    }
    if (e->view != NULL) {
        wgpu_bg_cache_invalidate_view_indexed(e);
        wgpuTextureViewRelease(e->view);
    }
    if (e->tex != NULL) {
        wgpuTextureRelease(e->tex);
    }
    e->tex = new_tex;
    e->view = new_view;
    e->w = level_w[0];
    e->h = level_h[0];
    e->levels = level_count;
    e->draw_epoch = 0;   /* WEB-053: replacement objects carry no recorded reads */
    return true;
}

static WGPUSampler wgpu_get_sampler(bool linear_filter, uint32_t cms, uint32_t cmt, bool mips) {
    cms = wgpu_cm_key(cms);
    cmt = wgpu_cm_key(cmt);
    for (int i = 0; i < s_sampler_n; ++i) {
        if (s_samplers[i].linear == (int)linear_filter &&
            s_samplers[i].mips == (int)mips &&
            s_samplers[i].cms == cms && s_samplers[i].cmt == cmt) {
            return s_samplers[i].sampler;
        }
    }
    WGPUSamplerDescriptor sd = {0};
    sd.addressModeU = wgpu_cm_to_address(cms);
    sd.addressModeV = wgpu_cm_to_address(cmt);
    sd.addressModeW = WGPUAddressMode_ClampToEdge;
    sd.magFilter = linear_filter ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    sd.minFilter = linear_filter ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    /* mdkr64: a texture carrying a CPU-built chain gets trilinear sampling and
     * an open LOD range. Everything else keeps the historical single-level
     * clamp, so 2D/TEXRECT content is unaffected. */
    sd.mipmapFilter = mips ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    sd.lodMinClamp = 0.0f;
    sd.lodMaxClamp = mips ? 32.0f : 0.0f;
    sd.maxAnisotropy = 1;
    /* Video.AnisotropicFiltering (remaster): resolve grazing-angle texture streak.
     * WebGPU requires all-Linear filters when maxAnisotropy>1, so only the linear
     * samplers are upgraded; nearest/point materials stay exactly as the N64 path.
     * gfx_pc.c gives 3-point (bilerp) materials a linear sampler when aniso is on,
     * and the WGSL generator emits hardware textureSample for them so this engages. */
    if (linear_filter && g_pcTextureAnisotropy > 1) {
        int aniso = g_pcTextureAnisotropy > 16 ? 16 : g_pcTextureAnisotropy;
        sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
        sd.maxAnisotropy = (uint16_t)aniso;
    }
    WGPUSampler s = WGPU_FAULT_CREATE(
        MATERIAL_SAMPLER, wgpuDeviceCreateSampler(s_device, &sd));
    if (s == NULL) {
        fprintf(stderr, "[webgpu] material sampler creation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not allocate a required material "
            "sampler. Reload the page to continue from the last persisted save.");
        return NULL;
    }
    if (s_sampler_n < (int)(sizeof(s_samplers) / sizeof(s_samplers[0]))) {
        s_samplers[s_sampler_n].linear = (int)linear_filter;
        s_samplers[s_sampler_n].mips = (int)mips;
        s_samplers[s_sampler_n].cms = cms;
        s_samplers[s_sampler_n].cmt = cmt;
        s_samplers[s_sampler_n].sampler = s;
        s_sampler_n++;
        if (s_sampler_n > s_sampler_high_water) {
            s_sampler_high_water = s_sampler_n;
        }
    } else {
        /* With normalized address modes there are only 2*2*3*3=36 possible
         * keys, so the 64-entry table cannot fill without state corruption. */
        fprintf(stderr,
                "[webgpu] sampler cache invariant failed (%d entries)\n",
                s_sampler_n);
        wgpuSamplerRelease(s);
        wgpu_runtime_fatal(
            "The graphics sampler cache became inconsistent. Reload the page "
            "to continue from the last persisted save.");
        return NULL;
    }
    return s;
}

static void wgpu_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    if (tile < 0 || tile > 1 || !s_ready) {
        return;
    }
    {
        struct WgpuTexEntry *e = wgpu_tex_lookup(s_bound_tex[tile]);
        bool mips = (e != NULL && e->levels > 1) && !g_gfxSamplerLod0Only;
        s_bound_sampler[tile] = wgpu_get_sampler(linear_filter, cms, cmt, mips);
    }
}

/* ------------------------------------------------------------------------
 * Vtable: pipeline state (recorded here; baked into the pipeline cache key)
 * ---------------------------------------------------------------------- */
static void wgpu_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare,
                                bool depth_source_prim, uint16_t zmode) {
    (void)depth_source_prim;
    s_depth_test = depth_test;
    s_depth_update = depth_update;
    s_depth_compare = depth_compare;
    s_zmode = zmode;
}
static void wgpu_set_viewport(int x, int y, int width, int height) {
    s_vp_x = x; s_vp_y = y; s_vp_w = width; s_vp_h = height;
}
static void wgpu_set_scissor(int x, int y, int width, int height) {
    s_sc_x = x; s_sc_y = y; s_sc_w = width; s_sc_h = height; s_sc_set = true;
}
static void wgpu_set_shadow_view(int view_index) {
    s_shadow_receiver_view = view_index;
}
static void wgpu_set_blend_mode(enum GfxBlendMode mode) { s_cur_blend = mode; }

/* Lazily create the draw fallbacks (1x1 white texture, nearest sampler) + the
 * per-frame bump vertex buffer (declared with the frame state above). Each draw
 * appends its buf_vbo at a fresh offset so all draws in the frame's single
 * render pass read distinct data (queue writes are ordered before submit). */
static void wgpu_ensure_draw_resources(void) {
    if (s_white_view == NULL) {
        WGPUTextureDescriptor td = {0};
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        td.dimension = WGPUTextureDimension_2D;
        td.size.width = 1; td.size.height = 1; td.size.depthOrArrayLayers = 1;
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.mipLevelCount = 1; td.sampleCount = 1;
        WGPUTexture new_tex = WGPU_FAULT_CREATE(
            WHITE_TEXTURE, wgpuDeviceCreateTexture(s_device, &td));
        WGPUTextureView new_view = NULL;
        if (new_tex != NULL) {
            uint8_t white[4] = {255, 255, 255, 255};
            WGPUTexelCopyTextureInfo dst = {0};
            dst.texture = new_tex; dst.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyBufferLayout lay = {0};
            lay.bytesPerRow = 4; lay.rowsPerImage = 1;
            WGPUExtent3D ext = {1, 1, 1};
            wgpuQueueWriteTexture(s_queue, &dst, white, 4, &lay, &ext);
            new_view = WGPU_FAULT_CREATE(
                WHITE_VIEW, wgpuTextureCreateView(new_tex, NULL));
        }
        if (new_view == NULL) {
            if (new_tex != NULL) {
                wgpuTextureRelease(new_tex);
            }
            fprintf(stderr, "[webgpu] fallback white texture creation failed\n");
            wgpu_runtime_fatal(
                "The graphics backend could not allocate its fallback texture. "
                "Reload the page to continue from the last persisted save.");
            return;
        }
        s_white_tex = new_tex;
        s_white_view = new_view;
    }
    if (s_default_sampler == NULL) {
        WGPUSamplerDescriptor sd = {0};
        sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_Repeat;
        sd.magFilter = sd.minFilter = WGPUFilterMode_Nearest;
        sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sd.maxAnisotropy = 1;
        s_default_sampler = WGPU_FAULT_CREATE(
            DEFAULT_SAMPLER, wgpuDeviceCreateSampler(s_device, &sd));
        if (s_default_sampler == NULL) {
            fprintf(stderr, "[webgpu] fallback sampler creation failed\n");
            wgpu_runtime_fatal(
                "The graphics backend could not allocate its fallback sampler. "
                "Reload the page to continue from the last persisted save.");
            return;
        }
    }
    if (s_vbuf == NULL) {
        uint32_t initial_cap = WGPU_VBUF_INITIAL_CAP;
        const char *forced = getenv("MDKR_TEST_WEBGPU_VERTEX_SEGMENT_BYTES");
        if (forced != NULL && forced[0] != '\0') {
            char *end = NULL;
            unsigned long parsed = strtoul(forced, &end, 10);
            if (end != forced && *end == '\0' && parsed >= 256 &&
                parsed <= UINT32_MAX - 255u) {
                initial_cap = ((uint32_t)parsed + 255u) & ~255u;
            }
        }
        WGPUBufferDescriptor bd = {0};
        bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        bd.size = initial_cap;
        s_vbuf = WGPU_FAULT_CREATE(
            VERTEX_BUFFER, wgpuDeviceCreateBuffer(s_device, &bd));
        if (s_vbuf == NULL) {
            fprintf(stderr, "[webgpu] vertex-buffer allocation failed\n");
            wgpu_runtime_fatal(
                "The graphics backend could not allocate its vertex buffer. "
                "Reload the page to continue from the last persisted save.");
            return;
        }
        s_vbuf_cap = initial_cap;
        if (s_vbuf_cap > s_vbuf_cap_high_water) {
            s_vbuf_cap_high_water = s_vbuf_cap;
        }
        /* WEB-023: shadow the vertex buffer CPU-side (calloc → padding gaps defined).
         * NULL is tolerated — wgpu_vbuf_stage falls back to per-batch writeBuffer. */
        if (s_vbuf_shadow == NULL) {
            s_vbuf_shadow = (uint8_t *)calloc(1, s_vbuf_cap);
            if (s_vbuf_shadow != NULL) {
                s_vbuf_shadow_cap = s_vbuf_cap;
            }
        }
    }
}

/*
 * Reserve one aligned vertex range. A frame may span multiple GPU buffers: when
 * the current segment fills, queue its staged bytes, publish a fresh segment,
 * and reset the segment-local offset. Commands already recorded against the old
 * buffer retain it through submit, so no earlier draw is copied or rewritten.
 * A single unusually large batch grows the next segment geometrically.
 */
static bool wgpu_vbuf_reserve(uint32_t bytes, uint32_t *offset) {
    uint32_t aligned;
    if (offset == NULL || bytes > UINT32_MAX - 3u) {
        return false;
    }
    aligned = (bytes + 3u) & ~3u;
    if (s_vbuf == NULL || s_vbuf_cap == 0) {
        return false;
    }
    if (s_vbuf_frame_segments == 0) {
        s_vbuf_frame_segments = 1;
    }
    if (aligned > s_vbuf_cap - s_vbuf_off) {
        uint32_t next_cap = s_vbuf_cap;
        while (next_cap < aligned) {
            if (next_cap > UINT32_MAX / 2u) {
                next_cap = aligned;
                break;
            }
            next_cap *= 2u;
        }
        WGPUBufferDescriptor bd = {0};
        bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        bd.size = next_cap;
        WGPUBuffer next = WGPU_FAULT_CREATE(
            VERTEX_BUFFER, wgpuDeviceCreateBuffer(s_device, &bd));
        if (next == NULL) {
            fprintf(stderr,
                    "[webgpu] vertex-buffer segment allocation failed "
                    "(need=%u cap=%u)\n",
                    aligned, next_cap);
            wgpu_runtime_fatal(
                "The graphics backend could not grow its vertex stream. Reload "
                "the page to continue from the last persisted save.");
            return false;
        }

        if (s_vbuf_shadow != NULL && s_vbuf_off > 0) {
            wgpuQueueWriteBuffer(
                s_queue, s_vbuf, 0, s_vbuf_shadow, s_vbuf_off);
        }
        if (next_cap > s_vbuf_shadow_cap && s_vbuf_shadow != NULL) {
            uint8_t *grown =
                (uint8_t *)realloc(s_vbuf_shadow, (size_t)next_cap);
            if (grown != NULL) {
                memset(grown + s_vbuf_shadow_cap, 0,
                       (size_t)next_cap - s_vbuf_shadow_cap);
                s_vbuf_shadow = grown;
                s_vbuf_shadow_cap = next_cap;
            } else {
                free(s_vbuf_shadow);
                s_vbuf_shadow = NULL;
                s_vbuf_shadow_cap = 0;
            }
        }
        /*
         * Recorded SetVertexBuffer calls and the queued write retain the old
         * buffer. Releasing our handle now prevents one leaked wrapper per
         * segment without invalidating the commands already recorded.
         */
        wgpuBufferRelease(s_vbuf);
        s_vbuf = next;
        s_vbuf_cap = next_cap;
        s_vbuf_off = 0;
        s_vbuf_frame_segments++;
        if (s_vbuf_cap > s_vbuf_cap_high_water) {
            s_vbuf_cap_high_water = s_vbuf_cap;
        }
    }
    *offset = s_vbuf_off;
    s_vbuf_off += aligned;
    s_vbuf_frame_bytes += aligned;
    if (s_vbuf_frame_bytes > s_vbuf_bytes_high_water) {
        s_vbuf_bytes_high_water = s_vbuf_frame_bytes;
    }
    if (s_vbuf_frame_segments > s_vbuf_segments_high_water) {
        s_vbuf_segments_high_water = s_vbuf_frame_segments;
    }
    return true;
}

/* WEB-023: stage a batch's vertex bytes into the CPU shadow (uploaded once in
 * wgpu_end_frame). Falls back to an immediate per-batch queue write when the shadow
 * malloc failed. voff/bytes are already validated against s_vbuf_cap by the
 * caller's bump-allocator guard, so the memcpy is in-bounds. */
static void wgpu_vbuf_stage(uint32_t voff, const void *src, uint32_t bytes) {
    if (s_vbuf_shadow != NULL) {
        memcpy(s_vbuf_shadow + voff, src, bytes);
    } else {
        wgpuQueueWriteBuffer(s_queue, s_vbuf, voff, src, bytes);
    }
}

/* RDP memory-blend support: split the open scene pass and copy the scene target
 * into the snapshot texture so the next draw's shader can sample the current
 * "memory color" (WebGPU cannot read the render target inside its own pass).
 * The resumed pass Loads both color and depth — no state is lost; viewport,
 * scissor, pipeline and bind group are re-applied per draw anyway. This is the
 * per-batch analogue of gfx_opengl.c's glCopyTexSubImage2D snapshot (one copy
 * per same-material XLU batch, PERFORMANCE_PLAN.md M1). */
/* Compute the batch's screen-space bounding rect (top-down texture coords,
 * padded for the ±0.5px coverage taps) from the interleaved VBO's clip
 * positions. Returns false when any vertex is un-projectable (w<=0) — the
 * caller then copies the whole target. Ports the intent of gfx_opengl.c's
 * gfx_opengl_compute_batch_snapshot_rect: bound the snapshot copy to the
 * pixels the batch can actually sample instead of the full scene target
 * (a full copy at fullscreen retina is tens of MB per glass/fence batch —
 * a measured 1%-low contributor). */
static bool wgpu_batch_snapshot_rect(const float *buf_vbo, size_t buf_vbo_len,
                                     int stride_floats,
                                     uint32_t *out_x, uint32_t *out_y,
                                     uint32_t *out_w, uint32_t *out_h) {
    const uint32_t target_w = wgpu_draw_target_w();
    const uint32_t target_h = wgpu_draw_target_h();
    if (buf_vbo == NULL || stride_floats < 4 || buf_vbo_len < (size_t)stride_floats) {
        return false;
    }
    float min_px = 1e30f, max_px = -1e30f;
    float min_gl = 1e30f, max_gl = -1e30f;   /* bottom-up pixel y */
    for (size_t off = 0; off + 4 <= buf_vbo_len; off += (size_t)stride_floats) {
        float w = buf_vbo[off + 3];
        if (!(w > 0.0f)) {
            return false;   /* behind the eye — bail to the full-target copy */
        }
        float ndc_x = buf_vbo[off + 0] / w;
        float ndc_y = buf_vbo[off + 1] / w;
        float px = (float)s_vp_x + (ndc_x + 1.0f) * 0.5f * (float)s_vp_w;
        float py = (float)s_vp_y + (ndc_y + 1.0f) * 0.5f * (float)s_vp_h;
        if (px < min_px) min_px = px;
        if (px > max_px) max_px = px;
        if (py < min_gl) min_gl = py;
        if (py > max_gl) max_gl = py;
    }
    if (min_px > max_px || min_gl > max_gl) {
        return false;
    }
    /* Pad for the 8-tap coverage offsets + rounding, flip to top-down, clamp. */
    int x0 = (int)min_px - 2;
    int x1 = (int)max_px + 3;
    int y0 = (int)target_h - ((int)max_gl + 3);   /* top-down top edge */
    int y1 = (int)target_h - ((int)min_gl - 2);   /* top-down bottom edge */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)target_w) x1 = (int)target_w;
    if (y1 > (int)target_h) y1 = (int)target_h;
    if (x1 <= x0 || y1 <= y0) {
        return false;   /* fully clipped — nothing the shader can sample */
    }
    *out_x = (uint32_t)x0;
    *out_y = (uint32_t)y0;
    *out_w = (uint32_t)(x1 - x0);
    *out_h = (uint32_t)(y1 - y0);
    return true;
}

static bool wgpu_snapshot_scene_for_memory_blend(const float *buf_vbo,
                                                 size_t buf_vbo_len,
                                                 int stride_floats) {
    const uint32_t target_w = wgpu_draw_target_w();
    const uint32_t target_h = wgpu_draw_target_h();
    WGPUTexture target_tex = wgpu_draw_target_tex();
    WGPUTextureView target_view = wgpu_draw_target_view();
    WGPUTextureView target_depth = wgpu_draw_depth_view();
    WGPUTexture *snapshot_tex = s_output_overlay_active
        ? &s_output_snap_tex : &s_snap_tex;
    WGPUTextureView *snapshot_view = s_output_overlay_active
        ? &s_output_snap_view : &s_snap_view;
    uint32_t *snapshot_w = s_output_overlay_active
        ? &s_output_snap_w : &s_snap_w;
    uint32_t *snapshot_h = s_output_overlay_active
        ? &s_output_snap_h : &s_snap_h;

    if (s_encoder == NULL || s_pass == NULL || target_tex == NULL ||
        target_view == NULL || target_depth == NULL ||
        target_w == 0 || target_h == 0) {
        return false;
    }
    if (*snapshot_view == NULL ||
        *snapshot_w != target_w || *snapshot_h != target_h) {
        WGPUTextureDescriptor td = {0};
        td.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
        td.dimension = WGPUTextureDimension_2D;
        td.size.width = target_w; td.size.height = target_h;
        td.size.depthOrArrayLayers = 1;
        td.format = s_surface_format;   /* must match the scene for T2T copy */
        td.mipLevelCount = 1; td.sampleCount = 1;
        WGPUTexture new_tex = WGPU_FAULT_CREATE(
            SNAPSHOT_TEXTURE, wgpuDeviceCreateTexture(s_device, &td));
        WGPUTextureView new_view = new_tex
            ? WGPU_FAULT_CREATE(
                SNAPSHOT_VIEW, wgpuTextureCreateView(new_tex, NULL))
            : NULL;
        if (new_view == NULL) {
            if (new_tex != NULL) {
                wgpuTextureRelease(new_tex);
            }
            fprintf(stderr,
                    "[webgpu] transactional memory-snapshot allocation failed\n");
            wgpu_runtime_fatal(
                "The graphics backend could not allocate a memory-blend "
                "snapshot. Reload the page to continue from the last persisted "
                "save.");
            return false;
        }
        if (*snapshot_view != NULL) {
            wgpu_bg_cache_invalidate_view(*snapshot_view);
            wgpuTextureViewRelease(*snapshot_view);
        }
        if (*snapshot_tex != NULL) {
            wgpuTextureRelease(*snapshot_tex);
        }
        *snapshot_tex = new_tex;
        *snapshot_view = new_view;
        *snapshot_w = target_w;
        *snapshot_h = target_h;
    }
    if (*snapshot_view == NULL) {
        return false;
    }
    if (s_snap_sampler == NULL) {
        WGPUSamplerDescriptor sd = {0};
        sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_ClampToEdge;
        sd.magFilter = sd.minFilter = WGPUFilterMode_Nearest;   /* GL snapshot is GL_NEAREST */
        sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sd.maxAnisotropy = 1;
        s_snap_sampler = WGPU_FAULT_CREATE(
            SNAPSHOT_SAMPLER, wgpuDeviceCreateSampler(s_device, &sd));
        if (s_snap_sampler == NULL) {
            fprintf(stderr, "[webgpu] memory-snapshot sampler creation failed\n");
            wgpu_runtime_fatal(
                "The graphics backend could not allocate a memory-blend "
                "sampler. Reload the page to continue from the last persisted "
                "save.");
            return false;
        }
    }

    wgpuRenderPassEncoderEnd(s_pass);
    wgpuRenderPassEncoderRelease(s_pass);
    s_pass = NULL;

    /* Copy only the batch's padded screen rect when computable (same origin in
     * src and dst keeps the snapshot 1:1 with the target, so the shader's
     * fragcoord-based memoryUv stays valid); whole target as the fallback. */
    uint32_t rx = 0, ry = 0, rw = target_w, rh = target_h;
    (void)wgpu_batch_snapshot_rect(buf_vbo, buf_vbo_len, stride_floats,
                                   &rx, &ry, &rw, &rh);
    WGPUTexelCopyTextureInfo cs = {0};
    cs.texture = target_tex; cs.aspect = WGPUTextureAspect_All;
    cs.origin.x = rx; cs.origin.y = ry;
    WGPUTexelCopyTextureInfo cd = {0};
    cd.texture = *snapshot_tex; cd.aspect = WGPUTextureAspect_All;
    cd.origin.x = rx; cd.origin.y = ry;
    WGPUExtent3D ext = { rw, rh, 1 };
    wgpuCommandEncoderCopyTextureToTexture(s_encoder, &cs, &cd, &ext);

    WGPURenderPassColorAttachment att = {0};
    att.view = target_view;
    att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    att.loadOp = WGPULoadOp_Load;
    att.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDepthStencilAttachment depth = {0};
    depth.view = target_depth;
    depth.depthLoadOp = WGPULoadOp_Load;
    depth.depthStoreOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp = {0};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;
    rp.depthStencilAttachment = &depth;
    s_pass = WGPU_FAULT_CREATE(
        SNAPSHOT_PASS, wgpuCommandEncoderBeginRenderPass(s_encoder, &rp));
    if (s_pass == NULL) {
        wgpuCommandEncoderRelease(s_encoder);
        s_encoder = NULL;
        s_frame_open = false;
        fprintf(stderr, "[webgpu] memory-snapshot resume pass failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not resume the scene after a "
            "memory-blend snapshot. Reload the page to continue from the last "
            "persisted save.");
        return false;
    }
    /* WEB-023-lite: a NEW pass encoder inherits none of the previous pass's
     * viewport/scissor — the next draw must re-apply, so clear the dedup flags. */
    wgpu_reset_pass_dynamic_state();
    return true;
}

/* Create a lazily-allocated 16-byte uniform buffer (or return the existing one).
 * Shared by the ring and overflow slots. */
static WGPUBuffer wgpu_diag_ubo_alloc(WGPUBuffer existing) {
    if (existing != NULL) {
        return existing;
    }
    WGPUBufferDescriptor bd = {0};
    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    bd.size = 16;
    return WGPU_FAULT_CREATE(
        DIAG_UNIFORM, wgpuDeviceCreateBuffer(s_device, &bd));
}

/* Fetch (or write) a 16-byte viewport UBO for the coverage-wrap shader. The
 * value is the GL-convention viewport exactly as gfx_opengl.c uploads it
 * (uDiagViewport = glGetIntegerv(GL_VIEWPORT) = bottom-up origin).
 *
 * WEB-051: every distinct value this frame gets its own buffer. The common path
 * (<= WGPU_DIAG_UBO_RING distinct viewports; usually 1) is allocation-free. On
 * overflow we GROW a heap array of extra buffers instead of overwriting an
 * occupied slot — wgpuQueueWriteBuffer runs before the command buffer, so
 * reusing a slot already referenced by an earlier draw would retroactively
 * rewrite that draw's viewport. */
static WGPUBuffer wgpu_diag_viewport_ubo(void) {
    float vp[4] = { (float)s_vp_x, (float)s_vp_y, (float)s_vp_w, (float)s_vp_h };
    /* Dedup against everything written this frame (ring first, then overflow). */
    for (int i = 0; i < s_diag_ubo_used; i++) {
        if (i < WGPU_DIAG_UBO_RING) {
            if (s_diag_ubo[i] != NULL && memcmp(s_diag_ubo_val[i], vp, sizeof(vp)) == 0) {
                return s_diag_ubo[i];
            }
        } else {
            int j = i - WGPU_DIAG_UBO_RING;
            if (s_diag_ubo_ext[j].buf != NULL &&
                memcmp(s_diag_ubo_ext[j].val, vp, sizeof(vp)) == 0) {
                return s_diag_ubo_ext[j].buf;
            }
        }
    }

    WGPUBuffer *pbuf;
    float      *pval;
    if (s_diag_ubo_used < WGPU_DIAG_UBO_RING) {
        pbuf = &s_diag_ubo[s_diag_ubo_used];
        pval = s_diag_ubo_val[s_diag_ubo_used];
    } else {
        int j = s_diag_ubo_used - WGPU_DIAG_UBO_RING;
        if (j >= s_diag_ubo_ext_cap) {
            int ncap = s_diag_ubo_ext_cap ? s_diag_ubo_ext_cap * 2 : WGPU_DIAG_UBO_RING;
            struct WgpuDiagUbo *n =
                (struct WgpuDiagUbo *)realloc(s_diag_ubo_ext, (size_t)ncap * sizeof(*n));
            if (n == NULL) {
                fprintf(stderr,
                        "[webgpu] diagnostic uniform index growth failed\n");
                wgpu_runtime_fatal(
                    "The graphics backend ran out of memory while indexing "
                    "draw uniforms. Reload the page to continue from the last "
                    "persisted save.");
                return NULL;
            }
            memset(n + s_diag_ubo_ext_cap, 0,
                   (size_t)(ncap - s_diag_ubo_ext_cap) * sizeof(*n));
            s_diag_ubo_ext = n;
            s_diag_ubo_ext_cap = ncap;
        }
        pbuf = &s_diag_ubo_ext[j].buf;
        pval = s_diag_ubo_ext[j].val;
    }

    *pbuf = wgpu_diag_ubo_alloc(*pbuf);
    if (*pbuf == NULL) {
        fprintf(stderr, "[webgpu] diagnostic uniform allocation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not allocate a required draw uniform. "
            "Reload the page to continue from the last persisted save.");
        return NULL;
    }
    wgpuQueueWriteBuffer(s_queue, *pbuf, 0, vp, sizeof(vp));
    memcpy(pval, vp, sizeof(vp));
    s_diag_ubo_used++;
    return *pbuf;
}

/* E28: fetch (or write) this draw group's 16-byte noise uniform, carrying
 * {frame seed, window height} exactly as gfx_opengl_set_uniforms() uploads
 * {frame_count, current_height}. The height is the VIEWPORT height (the value
 * gfx_opengl_set_viewport stores), not the render-target height, so split-screen
 * viewports hash the same way on both backends.
 *
 * Deduplicated by value and grown on overflow, for the reason the diag viewport
 * ring above documents: wgpuQueueWriteBuffer runs before the command buffer, so
 * an occupied slot must never be rewritten. Deduplication also keeps the draw
 * bind-group cache effective — repeated materials inside one viewport resolve to
 * the same buffer pointer and therefore the same cache key. */
static WGPUBuffer wgpu_noise_ubo(void) {
    float params[4] = { (float)s_noise_frame, (float)s_vp_h, 0.0f, 0.0f };
    for (int i = 0; i < s_noise_ubo_used; i++) {
        if (i < WGPU_NOISE_UBO_RING) {
            if (s_noise_ubo[i] != NULL &&
                memcmp(s_noise_ubo_val[i], params, sizeof(params)) == 0) {
                return s_noise_ubo[i];
            }
        } else {
            int j = i - WGPU_NOISE_UBO_RING;
            if (s_noise_ubo_ext[j].buf != NULL &&
                memcmp(s_noise_ubo_ext[j].val, params, sizeof(params)) == 0) {
                return s_noise_ubo_ext[j].buf;
            }
        }
    }

    WGPUBuffer *pbuf;
    float      *pval;
    if (s_noise_ubo_used < WGPU_NOISE_UBO_RING) {
        pbuf = &s_noise_ubo[s_noise_ubo_used];
        pval = s_noise_ubo_val[s_noise_ubo_used];
    } else {
        int j = s_noise_ubo_used - WGPU_NOISE_UBO_RING;
        if (j >= s_noise_ubo_ext_cap) {
            int ncap = s_noise_ubo_ext_cap ? s_noise_ubo_ext_cap * 2
                                           : WGPU_NOISE_UBO_RING;
            struct WgpuNoiseUbo *n = (struct WgpuNoiseUbo *)realloc(
                s_noise_ubo_ext, (size_t)ncap * sizeof(*n));
            if (n == NULL) {
                fprintf(stderr, "[webgpu] noise uniform index growth failed\n");
                wgpu_runtime_fatal(
                    "The graphics backend ran out of memory while indexing "
                    "draw uniforms. Reload the page to continue from the last "
                    "persisted save.");
                return NULL;
            }
            memset(n + s_noise_ubo_ext_cap, 0,
                   (size_t)(ncap - s_noise_ubo_ext_cap) * sizeof(*n));
            s_noise_ubo_ext = n;
            s_noise_ubo_ext_cap = ncap;
        }
        pbuf = &s_noise_ubo_ext[j].buf;
        pval = s_noise_ubo_ext[j].val;
    }

    if (*pbuf == NULL) {
        WGPUBufferDescriptor bd = {0};
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bd.size = 16;
        *pbuf = WGPU_FAULT_CREATE(
            NOISE_BUFFER, wgpuDeviceCreateBuffer(s_device, &bd));
    }
    if (*pbuf == NULL) {
        fprintf(stderr, "[webgpu] noise-uniform allocation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not allocate its frame uniform. "
            "Reload the page to continue from the last persisted save.");
        return NULL;
    }
    wgpuQueueWriteBuffer(s_queue, *pbuf, 0, params, sizeof(params));
    memcpy(pval, params, sizeof(params));
    s_noise_ubo_used++;
    return *pbuf;
}

/* Clip a rect (x,y,w,h) to [0,maxw] x [0,maxh], zeroing degenerate extents.
 * Order matters: shift for a negative origin first, bail to empty if the origin
 * is at/past the far edge, THEN clip the extent to the bound, THEN a final
 * non-negative clamp — so a containment adjustment can never re-introduce a
 * negative extent that would cast to a ~4-billion uint32 in Set{Viewport,Scissor}. */
static void wgpu_clamp_rect(int *x, int *y, int *w, int *h, int maxw, int maxh) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x >= maxw || *y >= maxh) { *w = 0; *h = 0; return; }
    if (*x + *w > maxw) *w = maxw - *x;
    if (*y + *h > maxh) *h = maxh - *y;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

/* Clamp a Y-flipped viewport rect to a rect WebGPU will accept, AND report the
 * clip-space correction that keeps the requested viewport *transform* intact.
 *
 * WebGPU validates setViewport containment, so an out-of-range rect has to be
 * clamped -- but clamping changes the transform, whereas glViewport and Metal
 * keep the transform and merely clip the pixels. DKR relies on the GL/Metal
 * behaviour: mtx_ortho() deliberately sets vp.vscale[1] = width*2, asking for a
 * viewport 320 logical pixels tall on a 240-tall target -- (0,-40,320,320)
 * logical, (0,-160,1280,1280) at this port's 4x HiDPI scale. Clamping that to
 * the target cuts its half-height from 160 to 120, a uniform 0.75 vertical
 * squash about the screen centre. (Measured: FILE_SELECT's wooden buttons came
 * out 48 rows tall against the real ROM's 64.)
 *
 * So: rasterize with the clamped rect, but pre-apply the inverse affine to
 * clip-space x/y. In WebGPU's top-down framebuffer space, requiring the
 * requested rect R and the clamped rect C to land a vertex on the same pixel:
 *     Rx + (ndc_x+1)/2 * Rw  ==  Cx + (ndc'_x+1)/2 * Cw
 *   =>  ndc'_x = ndc_x * (Rw/Cw) + ((2*(Rx-Cx) + Rw)/Cw - 1)
 *     Ry + (1-ndc_y)/2 * Rh  ==  Cy + (1-ndc'_y)/2 * Ch
 *   =>  ndc'_y = ndc_y * (Rh/Ch) + (1 - (2*(Ry-Cy) + Rh)/Ch)
 * Geometry the rescale pushes outside the NDC cube maps outside the clamped
 * rect -- i.e. off-target -- so clipping it is correct.
 *
 * Returns true when the caller must apply (sx,bx,sy,by) to clip-space x/y. */
static bool wgpu_viewport_fix(int *x, int *y, int *w, int *h,
                              float *sx, float *bx, float *sy, float *by) {
    const int rx = *x, ry = *y, rw = *w, rh = *h;
    const uint32_t target_w = wgpu_draw_target_w();
    const uint32_t target_h = wgpu_draw_target_h();
    *sx = 1.0f; *bx = 0.0f; *sy = 1.0f; *by = 0.0f;

    if (rx >= 0 && ry >= 0 &&
        rx + rw <= (int)target_w && ry + rh <= (int)target_h) {
        return false;   /* already legal — transform is exact, nothing to do */
    }

    /* DAM-R1 instrumentation: log every rect the clamp has to touch. */
    {
        static int s_vp_trace = -1;
        if (s_vp_trace < 0) {
            s_vp_trace = getenv("GE007_WEBGPU_TRACE_VIEWPORT") != NULL ? 1 : 0;
        }
        if (s_vp_trace) {
            static int s_vp_trace_count = 0;
            if (s_vp_trace_count < 64) {
                s_vp_trace_count++;
                fprintf(stderr,
                        "[WGPU-VP] out-of-range viewport pre-clamp: raw=(%d,%d %dx%d) "
                        "flipped=(%d,%d %dx%d) scene=%ux%u\n",
                        s_vp_x, s_vp_y, s_vp_w, s_vp_h,
                        rx, ry, rw, rh, target_w, target_h);
            }
        }
    }

    wgpu_clamp_rect(x, y, w, h, (int)target_w, (int)target_h);
    if (*w <= 0 || *h <= 0) {
        return false;   /* degenerate — the draw is skipped anyway */
    }
    *sx = (float)rw / (float)*w;
    *bx = (2.0f * (float)(rx - *x) + (float)rw) / (float)*w - 1.0f;
    *sy = (float)rh / (float)*h;
    *by = 1.0f - (2.0f * (float)(ry - *y) + (float)rh) / (float)*h;
    return *sx != 1.0f || *bx != 0.0f || *sy != 1.0f || *by != 0.0f;
}

/* Scratch for the corrected copy of a batch's vertex data (only allocated when a
 * viewport actually needs correcting, i.e. on DKR's ortho passes). */
static float *s_vp_fix_buf;
static size_t s_vp_fix_cap;

static float *wgpu_vp_fix_scratch(size_t n_floats) {
    if (n_floats > s_vp_fix_cap) {
        if (n_floats > SIZE_MAX / sizeof(float)) {
            return NULL;
        }
        float *p = (float *)realloc(s_vp_fix_buf, n_floats * sizeof(float));
        if (p == NULL) return NULL;
        s_vp_fix_buf = p;
        s_vp_fix_cap = n_floats;
    }
    return s_vp_fix_buf;
}

static void wgpu_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    const uint32_t target_w = wgpu_draw_target_w();
    const uint32_t target_h = wgpu_draw_target_h();
    if (!s_frame_open || s_pass == NULL ||
        s_cur_shader == NULL || s_cur_shader->module == NULL ||
        buf_vbo == NULL || buf_vbo_len == 0 || buf_vbo_num_tris == 0) {
        return;
    }
    wgpu_ensure_draw_resources();
    if (s_vbuf == NULL) {
        return;
    }
    WGPURenderPipeline pipe = wgpu_pipeline_for(s_cur_shader, s_cur_blend);
    if (pipe == NULL) {
        return;
    }

    /* RDP memory-blend draw (glass / fence class): snapshot the scene so the
     * shader can sample the current framebuffer as the N64 memory color. If the
     * snapshot cannot be produced, skip the draw — an opaque mis-blend is worse
     * than a dropped XLU batch. */
    bool rdp_mem_draw = s_cur_shader->info.diag_rdp_memory_blend ||
                        s_cur_shader->info.diag_rdp_cvg_memory_blend;
    WGPUBuffer diag_ubo = NULL;
    if (rdp_mem_draw) {
        if (!wgpu_snapshot_scene_for_memory_blend(buf_vbo, buf_vbo_len,
                                                  s_cur_shader->info.num_floats)) {
            return;
        }
        if (s_cur_shader->info.diag_rdp_cvg_memory_blend) {
            diag_ubo = wgpu_diag_viewport_ubo();
            if (diag_ubo == NULL) {
                return;
            }
        }
    }
    /* E28: claimed before the bind-group key is built — it is part of that key,
     * because the buffer now varies with the viewport rather than being one
     * frame-global handle. */
    WGPUBuffer noise_ubo = NULL;
    if (s_cur_shader->info.uses_noise) {
        noise_ubo = wgpu_noise_ubo();
        if (noise_ubo == NULL) {
            return;
        }
    }

    /* Viewport, Y-flipped to WebGPU's top-left origin (GL/gfx_pc emit bottom-left;
     * mirrors mtl_draw_triangles' originY = fb_h - (y + h)), reduced to a rect
     * WebGPU will accept plus the clip-space correction that preserves the
     * transform the game asked for. Resolved BEFORE the vertex data is staged,
     * because the correction is applied to the staged copy. */
    int vp_x = s_vp_x, vp_y = (int)target_h - (s_vp_y + s_vp_h);
    int vp_w = s_vp_w, vp_h = s_vp_h;
    float fix_sx, fix_bx, fix_sy, fix_by;
    bool vp_fix = wgpu_viewport_fix(&vp_x, &vp_y, &vp_w, &vp_h,
                                   &fix_sx, &fix_bx, &fix_sy, &fix_by);
    /*
     * A rect that clamped away entirely covers no pixels, and SetViewport
     * rejects a zero extent. Dropping only the Set would leave the PREVIOUS
     * draw group's viewport live on the pass encoder and let this batch
     * rasterize into that group's rect, so the whole draw goes.
     */
    if (vp_w <= 0 || vp_h <= 0) {
        return;
    }

    /* Bump-allocate this batch's vertex data. */
    if (buf_vbo_len > UINT32_MAX / sizeof(float)) {
        fprintf(stderr, "[webgpu] draw vertex byte count overflow\n");
        wgpu_runtime_fatal(
            "A draw exceeds the graphics backend's vertex limits. Reload the "
            "page to continue from the last persisted save.");
        return;
    }
    uint32_t bytes = (uint32_t)(buf_vbo_len * sizeof(float));
    uint32_t voff = 0;
    if (!wgpu_vbuf_reserve(bytes, &voff)) {
        if (s_runtime_status != GFX_RENDERING_FATAL) {
            fprintf(stderr, "[webgpu] invalid vertex-stream reservation\n");
            wgpu_runtime_fatal(
                "A draw exceeds the graphics backend's vertex limits. Reload "
                "the page to continue from the last persisted save.");
        }
        return;
    }
    if (vp_fix) {
        /* x' = x*sx + bx*w, y' = y*sy + by*w — the clip-space form of the NDC
         * affine, so it stays correct under perspective. */
        const int stride = s_cur_shader->info.num_floats;
        float *fixed = (stride >= 4) ? wgpu_vp_fix_scratch(buf_vbo_len) : NULL;
        if (stride < 4) {
            wgpu_vbuf_stage(voff, buf_vbo, bytes);
        } else if (fixed == NULL) {
            fprintf(stderr,
                    "[webgpu] viewport-correction scratch allocation failed\n");
            wgpu_runtime_fatal(
                "The graphics backend ran out of memory while correcting a "
                "viewport. Reload the page to continue from the last persisted "
                "save.");
            return;
        } else {
            memcpy(fixed, buf_vbo, buf_vbo_len * sizeof(float));
            for (size_t off = 0; off + 4 <= buf_vbo_len; off += (size_t)stride) {
                float vw = fixed[off + 3];
                fixed[off + 0] = fixed[off + 0] * fix_sx + fix_bx * vw;
                fixed[off + 1] = fixed[off + 1] * fix_sy + fix_by * vw;
            }
            wgpu_vbuf_stage(voff, fixed, bytes);
        }
    } else {
        wgpu_vbuf_stage(voff, buf_vbo, bytes);
    }
    /* Bind group: each used texture's live view + sampler, falling back to a 1x1
     * white texture / nearest sampler when unset. Looked up in the 2,048-entry,
     * 4-way set-associative bind-group cache (s_bg_cache_tab, defined above at
     * the "Persistent draw bind-group cache" comment): a materials-worth of
     * concurrent (view, sampler) combos all stay resident across frames, so
     * this is a cache probe, not an allocation, on the common repeat-material
     * path. The key is on POINTER VALUES — safe only because every view/bgl
     * release purges matching entries first (wgpu_bg_cache_invalidate_view /
     * _bgl, the WEB-068 fix): a NEW object allocated at a RECYCLED handle
     * address would otherwise false-match a stale entry and sample the old
     * texture (the menu-glyph corruption class). */
    WGPUBindGroup bg = NULL;
    if (s_cur_shader->bgl != NULL) {
        WGPUTextureView v0 = s_white_view, v1 = s_white_view;
        WGPUSampler     m0 = s_default_sampler, m1 = s_default_sampler;
        /* PERF-019: track the texture entries whose views become v0/v1, so a newly
         * built bind group can register its cache slot in their reverse index. NULL
         * when the tile falls back to s_white_view (never released → no index needed). */
        struct WgpuTexEntry *e0 = NULL, *e1 = NULL;
        if (s_cur_shader->info.used_textures[0]) {
            struct WgpuTexEntry *e = wgpu_tex_lookup(s_bound_tex[0]);
            if (e != NULL && e->view != NULL) { v0 = e->view; e0 = e; }
            if (s_bound_sampler[0] != NULL) m0 = s_bound_sampler[0];
        }
        if (s_cur_shader->info.used_textures[1]) {
            struct WgpuTexEntry *e = wgpu_tex_lookup(s_bound_tex[1]);
            if (e != NULL && e->view != NULL) { v1 = e->view; e1 = e; }
            if (s_bound_sampler[1] != NULL) m1 = s_bound_sampler[1];
        }
        /* WEB-053: this draw is about to be recorded into the live encoder, so
         * these two textures are now read by a command buffer that has not been
         * submitted. Stamping here (rather than after the Draw) is deliberately
         * conservative: the few paths that bail out between here and the Draw
         * cost at most one extra texture recreate, never a missed hazard. */
        if (e0 != NULL) e0->draw_epoch = s_draw_epoch;
        if (e1 != NULL) e1->draw_epoch = s_draw_epoch;
        WGPUTextureView draw_snapshot_view = wgpu_draw_snapshot_view();
        WGPUTextureView shadow_view = NULL;
        WGPUSampler shadow_sampler = NULL;
        WGPUBuffer shadow_ubo = NULL;
        if (s_cur_shader->info.opt_sun_shadow) {
            if (s_shadow_receiver_view < 0 ||
                s_shadow_receiver_view >= (int)s_shadow_plan.view_count ||
                (g_pc_shadow_view_ready_mask &
                 (1u << (unsigned)s_shadow_receiver_view)) == 0 ||
                s_shadow_array_view == NULL ||
                s_shadow_sampler == NULL ||
                s_shadow_receiver_ubo[s_shadow_receiver_view] == NULL) {
                return;
            }
            shadow_view = s_shadow_array_view;
            shadow_sampler = s_shadow_sampler;
            shadow_ubo = s_shadow_receiver_ubo[s_shadow_receiver_view];
        }
        const void *key[WGPU_BG_KEY_COUNT] = {
            s_cur_shader->bgl, v0, m0, v1, m1,
            rdp_mem_draw
                ? (const void *)draw_snapshot_view
                : NULL,
            (const void *)diag_ubo,
            (const void *)shadow_view,
            (const void *)shadow_sampler,
            (const void *)shadow_ubo,
            (const void *)noise_ubo,
        };
        uint32_t set_base = (wgpu_bg_key_hash(key) & (WGPU_BG_CACHE / WGPU_BG_WAYS - 1)) * WGPU_BG_WAYS;
        for (int w = 0; w < WGPU_BG_WAYS; w++) {
            struct WgpuBgEntry *e = &s_bg_cache_tab[set_base + w];
            if (e->bg != NULL && memcmp(key, e->key, sizeof(key)) == 0) {
                bg = e->bg;   /* hit: reuse, no alloc this draw */
                break;
            }
        }
        if (bg == NULL) {
            WGPUBindGroupEntry ents[12];
            int ne = 0;
            for (int t = 0; t < 2; t++) {
                if (!s_cur_shader->info.used_textures[t]) continue;
                WGPUBindGroupEntry te = {0}; te.binding = (uint32_t)(t * 2);
                te.textureView = t == 0 ? v0 : v1; ents[ne++] = te;
                WGPUBindGroupEntry se = {0}; se.binding = (uint32_t)(t * 2 + 1);
                se.sampler = t == 0 ? m0 : m1; ents[ne++] = se;
            }
            if (rdp_mem_draw) {
                WGPUBindGroupEntry te = {0}; te.binding = 4;
                te.textureView = draw_snapshot_view; ents[ne++] = te;
                WGPUBindGroupEntry se = {0}; se.binding = 5;
                se.sampler = s_snap_sampler; ents[ne++] = se;
            }
            if (diag_ubo != NULL) {
                WGPUBindGroupEntry ue = {0}; ue.binding = 6;
                ue.buffer = diag_ubo; ue.size = 16; ents[ne++] = ue;
            }
            /* WEB-027 / E28: this draw group's noise uniform (binding 7). The
             * bgl carries this entry only for noise combiners, so a noise shader
             * with no textures still has a non-NULL bgl and reaches this build.
             * The buffer is a per-viewport ring slot claimed above and is part
             * of the cache key, so two viewports never share an entry.
             *
             * A noise BGL REQUIRES entry 7 — building the group without it
             * yields an ERROR OBJECT (not NULL) that the cache would retain and
             * every later draw would trip over. The claim above already dropped
             * the draw if the 16-byte allocation failed (≈ device lost). */
            if (s_cur_shader->info.uses_noise) {
                WGPUBindGroupEntry ue = {0}; ue.binding = 7;
                ue.buffer = noise_ubo; ue.size = 16; ents[ne++] = ue;
            }
            if (s_cur_shader->info.opt_dfdx_light) {
                if (s_light_ubo == NULL) {
                    return;
                }
                WGPUBindGroupEntry ue = {0}; ue.binding = 8;
                ue.buffer = s_light_ubo; ue.size = 16; ents[ne++] = ue;
            }
            if (s_cur_shader->info.opt_sun_shadow) {
                WGPUBindGroupEntry depth = {0};
                depth.binding = 9;
                depth.textureView = shadow_view;
                ents[ne++] = depth;
                WGPUBindGroupEntry compare = {0};
                compare.binding = 10;
                compare.sampler = shadow_sampler;
                ents[ne++] = compare;
                WGPUBindGroupEntry uniform = {0};
                uniform.binding = 11;
                uniform.buffer = shadow_ubo;
                uniform.size = WGPU_SHADOW_UNIFORM_SIZE;
                ents[ne++] = uniform;
            }
            WGPUBindGroupDescriptor bd = {0};
            bd.layout = s_cur_shader->bgl;
            bd.entryCount = (size_t)ne;
            bd.entries = ents;
            bg = WGPU_FAULT_CREATE(
                DRAW_BIND_GROUP,
                wgpuDeviceCreateBindGroup(s_device, &bd));
            if (bg != NULL) {
                /* Insert into the set: first empty way, else round-robin
                 * eviction (the in-flight pass holds its own ref on any
                 * evicted group until submit, so release here is safe). */
                struct WgpuBgEntry *victim = NULL;
                for (int w = 0; w < WGPU_BG_WAYS; w++) {
                    if (s_bg_cache_tab[set_base + w].bg == NULL) {
                        victim = &s_bg_cache_tab[set_base + w];
                        break;
                    }
                }
                if (victim == NULL) {
                    victim = &s_bg_cache_tab[set_base + (s_bg_cache_way & (WGPU_BG_WAYS - 1))];
                    s_bg_cache_way++;
                    wgpu_release_cached_bind_group(victim->bg);
                } else {
                    s_bg_cache_live++;
                    if (s_bg_cache_live > s_bg_cache_high_water) {
                        s_bg_cache_high_water = s_bg_cache_live;
                    }
                }
                memcpy(victim->key, key, sizeof(victim->key));
                victim->bg = bg;   /* the cache owns the ref */
                /* PERF-019: register this slot in the reverse index of the textures
                 * whose views it references (v0/v1), so releasing either view later
                 * invalidates just this slot instead of sweeping the full table.
                 * The evicted slot's stale reverse-index entries (if any) are filtered
                 * by re-verification at release time, so no cleanup is needed here. */
                uint32_t slot_idx = (uint32_t)(victim - s_bg_cache_tab);
                wgpu_tex_bg_ref_add(e0, slot_idx);
                if (e1 != e0) {
                    wgpu_tex_bg_ref_add(e1, slot_idx);   /* skip a duplicate when both tiles share a texture */
                }
            } else {
                fprintf(stderr, "[webgpu] material bind-group creation failed\n");
                wgpu_runtime_fatal(
                    "The graphics backend could not bind a required material. "
                    "Reload the page to continue from the last persisted save.");
                return;
            }
        }
    }

    /* Viewport (resolved above) + scissor. BOTH must be clamped to the scene
     * bounds: WebGPU *validates* setViewport/setScissorRect containment and a
     * single out-of-range rect invalidates the whole command buffer (black
     * frame), whereas GL/Metal silently clip — so the game may legitimately hand
     * us a viewport/scissor extending past the target (widescreen / split-screen,
     * and DKR's ortho pass). Clamping the SCISSOR only drops pixels, so it needs
     * no compensation; clamping the VIEWPORT would change the transform, which is
     * why wgpu_viewport_fix() returned a clip-space correction for it. */
    if (vp_w > 0 && vp_h > 0) {
        /* WEB-023-lite: skip the Set when the (clamped, Y-flipped) rect is
         * unchanged from the last one applied to this pass encoder. */
        if (!s_vp_applied || vp_x != s_vp_ax || vp_y != s_vp_ay ||
            vp_w != s_vp_aw || vp_h != s_vp_ah) {
            wgpuRenderPassEncoderSetViewport(s_pass, (float)vp_x, (float)vp_y,
                                             (float)vp_w, (float)vp_h, 0.0f, 1.0f);
            s_vp_applied = true;
            s_vp_ax = vp_x; s_vp_ay = vp_y; s_vp_aw = vp_w; s_vp_ah = vp_h;
        }
    }
    {
        int sx = s_sc_set ? s_sc_x : 0;
        int sw = s_sc_set ? s_sc_w : (int)target_w;
        int sh = s_sc_set ? s_sc_h : (int)target_h;
        int sy = s_sc_set ? ((int)target_h - (s_sc_y + s_sc_h)) : 0;
        wgpu_clamp_rect(&sx, &sy, &sw, &sh, (int)target_w, (int)target_h);
        /* WEB-023-lite: skip the redundant SetScissorRect (gfx_pc re-emits the
         * same rect every draw). Trackers are reset at each pass begin. */
        if (!s_sc_applied || sx != s_sc_ax || sy != s_sc_ay ||
            sw != s_sc_aw || sh != s_sc_ah) {
            wgpuRenderPassEncoderSetScissorRect(s_pass, (uint32_t)sx, (uint32_t)sy,
                                                (uint32_t)sw, (uint32_t)sh);
            s_sc_applied = true;
            s_sc_ax = sx; s_sc_ay = sy; s_sc_aw = sw; s_sc_ah = sh;
        }
    }

    /* PERF-014: skip the SetPipeline/SetBindGroup when unchanged from the last
     * draw applied to this pass encoder. gfx_pc re-issues full material setup per
     * draw group, so consecutive draws frequently repeat the same pipeline+bind
     * group; on web each redundant Set is a wasm↔JS crossing. The trackers are
     * reset to NULL at every s_pass begin (wgpu_reset_pass_dynamic_state), so the
     * first draw of a pass always issues both. `pipe` is non-NULL here — the
     * PERF-005 early-out above (wgpu_pipeline_for == NULL → return) ran before this
     * point, so a pending/failed pipeline never reaches, nor poisons, the tracker. */
    if (pipe != s_pipe_applied) {
        wgpuRenderPassEncoderSetPipeline(s_pass, pipe);
        s_pipe_applied = pipe;
    }
    if (bg != NULL && bg != s_bg_applied) {
        wgpuRenderPassEncoderSetBindGroup(s_pass, 0, bg, 0, NULL);
        s_bg_applied = bg;
    }
    wgpuRenderPassEncoderSetVertexBuffer(s_pass, 0, s_vbuf, voff, bytes);
    wgpuRenderPassEncoderDraw(s_pass, (uint32_t)(3 * buf_vbo_num_tris), 1, 0, 0);
    /* bg is owned by the bind-group cache (retained across draws); the pass holds
     * its own reference until submit, so we never release it here. */
}

/* ------------------------------------------------------------------------
 * Minimap / radar overlay (T4b) — a 2D screen-space pass into the scene target
 * after the main geometry, mirroring the GL (minimap_overlay.c direct draws) and
 * Metal (gfx_metal_draw_minimap_overlay) paths. Vertices are MinimapOverlayVertex
 * {x,y (pixels), r,g,b,a} (stride 24); the shader maps pixels -> NDC with a
 * screen-size uniform. Called from wgpu_end_frame after the main render pass.
 * ---------------------------------------------------------------------- */
static WGPURenderPipeline s_mm_pipe = NULL;
static WGPUBindGroupLayout s_mm_bgl = NULL;
static WGPUBindGroup s_mm_bg = NULL;
static WGPUBuffer s_mm_ubuf = NULL;

static const char *kMinimapWGSL =
    "struct MMIn { @location(0) pos : vec2<f32>, @location(1) color : vec4<f32> };\n"
    "struct MMOut { @builtin(position) clip : vec4<f32>, @location(0) color : vec4<f32> };\n"
    "struct MMU { screen : vec2<f32>, pad : vec2<f32> };\n"
    "@group(0) @binding(0) var<uniform> mmu : MMU;\n"
    "@vertex fn vs_main(in : MMIn) -> MMOut {\n"
    "  var o : MMOut;\n"
    "  let ndc = vec2<f32>((in.pos.x / mmu.screen.x) * 2.0 - 1.0, 1.0 - (in.pos.y / mmu.screen.y) * 2.0);\n"
    "  o.clip = vec4<f32>(ndc, 0.0, 1.0);\n"
    "  o.color = in.color;\n"
    "  return o;\n}\n"
    "@fragment fn fs_main(in : MMOut) -> @location(0) vec4<f32> { return in.color; }\n";

static bool wgpu_ensure_minimap(void) {
    WGPUShaderModule mod = NULL;
    WGPUBindGroupLayout bgl = NULL;
    WGPUBuffer ubuf = NULL;
    WGPUBindGroup bind_group = NULL;
    WGPUPipelineLayout layout = NULL;
    WGPURenderPipeline pipeline = NULL;

    if (s_mm_pipe != NULL) {
        return true;
    }
    if (!s_ready) {
        return false;
    }
    WGPUShaderSourceWGSL src = {0};
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = wgpu_sv(kMinimapWGSL);
    WGPUShaderModuleDescriptor smd = {0};
    smd.nextInChain = (WGPUChainedStruct *)&src;
    mod = WGPU_FAULT_CREATE(
        MINIMAP_MODULE, wgpuDeviceCreateShaderModule(s_device, &smd));
    if (mod == NULL) {
        goto fail;
    }

    WGPUBindGroupLayoutEntry ue = {0};
    ue.binding = 0;
    ue.visibility = WGPUShaderStage_Vertex;
    ue.buffer.type = WGPUBufferBindingType_Uniform;
    ue.buffer.minBindingSize = 16;
    WGPUBindGroupLayoutDescriptor bgld = {0};
    bgld.entryCount = 1;
    bgld.entries = &ue;
    bgl = WGPU_FAULT_CREATE(
        MINIMAP_BGL, wgpuDeviceCreateBindGroupLayout(s_device, &bgld));

    WGPUBufferDescriptor ubd = {0};
    ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubd.size = 16;
    ubuf = WGPU_FAULT_CREATE(
        MINIMAP_UNIFORM, wgpuDeviceCreateBuffer(s_device, &ubd));
    if (bgl == NULL || ubuf == NULL) {
        goto fail;
    }

    WGPUBindGroupEntry bge = {0};
    bge.binding = 0;
    bge.buffer = ubuf;
    bge.size = 16;
    WGPUBindGroupDescriptor bgd = {0};
    bgd.layout = bgl;
    bgd.entryCount = 1;
    bgd.entries = &bge;
    bind_group = WGPU_FAULT_CREATE(
        MINIMAP_BIND_GROUP, wgpuDeviceCreateBindGroup(s_device, &bgd));
    if (bind_group == NULL) {
        goto fail;
    }

    WGPUPipelineLayoutDescriptor pld = {0};
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &bgl;
    layout = WGPU_FAULT_CREATE(
        MINIMAP_LAYOUT, wgpuDeviceCreatePipelineLayout(s_device, &pld));
    if (layout == NULL) {
        goto fail;
    }

    WGPUVertexAttribute attrs[2] = {0};
    attrs[0].format = WGPUVertexFormat_Float32x2; attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
    attrs[1].format = WGPUVertexFormat_Float32x4; attrs[1].offset = 8;  attrs[1].shaderLocation = 1;
    WGPUVertexBufferLayout vbl = {0};
    vbl.stepMode = WGPUVertexStepMode_Vertex;
    vbl.arrayStride = 24;   /* MinimapOverlayVertex: 6 floats */
    vbl.attributeCount = 2;
    vbl.attributes = attrs;

    WGPUBlendState blend = {0};   /* standard alpha (the overlay is translucent) */
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    WGPUColorTargetState color = {0};
    color.format = s_surface_format;
    color.writeMask = WGPUColorWriteMask_All;
    color.blend = &blend;

    WGPUFragmentState fs = {0};
    fs.module = mod; fs.entryPoint = wgpu_sv("fs_main");
    fs.targetCount = 1; fs.targets = &color;
    WGPURenderPipelineDescriptor pd = {0};
    pd.layout = layout;
    pd.vertex.module = mod; pd.vertex.entryPoint = wgpu_sv("vs_main");
    pd.vertex.bufferCount = 1; pd.vertex.buffers = &vbl;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.primitive.unclippedDepth = s_unclipped_depth_supported ? WGPU_TRUE : WGPU_FALSE;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;   /* no depth-stencil: 2D overlay pass has no depth attachment */
    pipeline = WGPU_FAULT_CREATE(
        MINIMAP_PIPELINE, wgpuDeviceCreateRenderPipeline(s_device, &pd));
    if (pipeline == NULL) {
        goto fail;
    }

    s_mm_bgl = bgl;
    s_mm_ubuf = ubuf;
    s_mm_bg = bind_group;
    s_mm_pipe = pipeline;
    wgpuPipelineLayoutRelease(layout);
    wgpuShaderModuleRelease(mod);
    return true;

fail:
    if (pipeline != NULL) wgpuRenderPipelineRelease(pipeline);
    if (layout != NULL) wgpuPipelineLayoutRelease(layout);
    if (bind_group != NULL) wgpuBindGroupRelease(bind_group);
    if (ubuf != NULL) wgpuBufferRelease(ubuf);
    if (bgl != NULL) wgpuBindGroupLayoutRelease(bgl);
    if (mod != NULL) wgpuShaderModuleRelease(mod);
    fprintf(stderr, "[webgpu] transactional minimap resource creation failed\n");
    wgpu_runtime_fatal(
        "The graphics backend could not allocate its minimap pipeline. Reload "
        "the page to continue from the last persisted save.");
    return false;
}

/* Called by minimap_overlay.c's flush. Returns nonzero on success (matching the
 * Metal hook's convention). Draws the queued overlay vertices into the scene
 * target in a fresh load-op render pass, using the still-open frame encoder. */
int gfx_webgpu_draw_minimap_overlay(const void *vertices, size_t vertex_count,
                                    int fb_width, int fb_height,
                                    int scissor_enabled, int scissor_x, int scissor_y,
                                    int scissor_w, int scissor_h) {
    if (!s_ready || s_encoder == NULL || s_scene_view == NULL || s_vbuf == NULL ||
        vertices == NULL || vertex_count == 0 || fb_width <= 0 || fb_height <= 0) {
        return 0;
    }
    if (!wgpu_ensure_minimap()) {
        return 0;
    }
    /* WEB-014: honor the minimap clip rect. GL and Metal both apply it; WebGPU
     * previously voided all five params, so minimap geometry the layout clips
     * (lines/blips while moving or rotating) could draw outside the minimap
     * window over the game view — on the default backend everywhere. Y-flip to
     * WebGPU's top-left origin like Metal (sy = fb_h - (y+h)), clamp to the
     * present-target pixel bounds, and skip a
     * fully-clipped draw. Computed BEFORE the vbuf write so a clipped-away frame
     * costs nothing (mirrors gfx_metal's early return). */
    int mm_sc_x = 0, mm_sc_y = 0, mm_sc_w = 0, mm_sc_h = 0;
    if (scissor_enabled) {
        mm_sc_x = scissor_x;
        mm_sc_y = fb_height - (scissor_y + scissor_h);
        mm_sc_w = scissor_w;
        mm_sc_h = scissor_h;
        wgpu_clamp_rect(&mm_sc_x, &mm_sc_y, &mm_sc_w, &mm_sc_h,
                        fb_width, fb_height);
        if (mm_sc_w <= 0 || mm_sc_h <= 0) {
            return 1;   /* wholly clipped — nothing to draw, not a failure */
        }
    }
    /* Draw onto the present target (post-FX result when the filter ran, else the
     * raw scene) so the minimap sits on top of the graded frame — not tonemapped —
     * matching GL/Metal. s_present_target_view is set in wgpu_end_frame before this
     * hook fires; fall back to the scene view defensively. */
    WGPUTextureView mm_target = s_present_target_view ? s_present_target_view : s_scene_view;
    if (vertex_count > UINT32_MAX / 24u) {
        fprintf(stderr, "[webgpu] minimap vertex byte count overflow\n");
        wgpu_runtime_fatal(
            "The minimap exceeds the graphics backend's vertex limits. Reload "
            "the page to continue from the last persisted save.");
        return 0;
    }
    uint32_t bytes = (uint32_t)(vertex_count * 24u);
    uint32_t voff = 0;
    if (!wgpu_vbuf_reserve(bytes, &voff)) {
        if (s_runtime_status != GFX_RENDERING_FATAL) {
            fprintf(stderr, "[webgpu] invalid minimap vertex reservation\n");
            wgpu_runtime_fatal(
                "The minimap exceeds the graphics backend's vertex limits. "
                "Reload the page to continue from the last persisted save.");
        }
        return 0;
    }
    wgpu_vbuf_stage(voff, vertices, bytes);

    float u[4] = { (float)fb_width, (float)fb_height, 0.0f, 0.0f };
    wgpuQueueWriteBuffer(s_queue, s_mm_ubuf, 0, u, sizeof(u));

    WGPURenderPassColorAttachment att = {0};
    att.view = mm_target;
    att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    att.loadOp = WGPULoadOp_Load;    /* preserve the (post-FX) frame underneath */
    att.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp = {0};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;
    WGPURenderPassEncoder pass = WGPU_FAULT_CREATE(
        MINIMAP_PASS, wgpuCommandEncoderBeginRenderPass(s_encoder, &rp));
    if (pass == NULL) {
        fprintf(stderr, "[webgpu] minimap-pass creation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not begin its minimap pass. Reload the "
            "page to continue from the last persisted save.");
        return 0;
    }
    wgpuRenderPassEncoderSetPipeline(pass, s_mm_pipe);
    if (scissor_enabled) {   /* WEB-014: clip to the minimap window */
        wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)mm_sc_x, (uint32_t)mm_sc_y,
                                            (uint32_t)mm_sc_w, (uint32_t)mm_sc_h);
    }
    wgpuRenderPassEncoderSetBindGroup(pass, 0, s_mm_bg, 0, NULL);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, s_vbuf, voff, bytes);
    wgpuRenderPassEncoderDraw(pass, (uint32_t)vertex_count, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    return 1;
}

/* Submit the in-progress scene and immediately resume it with Load semantics.
 *
 * The ordinary WebGPU path intentionally records one command buffer for the
 * whole frame.  Draw-boundary diagnostics in gfx_pc.c are different: they call
 * read_framebuffer_rgb before and after a selected draw and require those reads
 * to observe the current command-stream position.  Without a partial submit,
 * readback can only see the previous completed frame, so every WebGPU
 * [TRI-PIXEL]/[SETTEX-PIXEL] row falsely reports pre == post.
 *
 * This slow path is reached only when a readback is actually requested.  The
 * normal gameplay path still performs the single end-of-frame vertex upload
 * and submit.  Vertex data must be uploaded before finishing this encoder:
 * WEB-023 normally defers that upload until end_frame, but the draws being
 * submitted here already reference the staged offsets. */
static bool wgpu_submit_live_scene_for_readback(void) {
    WGPUTextureView target_view = wgpu_draw_target_view();
    WGPUTextureView target_depth = wgpu_draw_depth_view();
    if (!s_frame_open || s_encoder == NULL || s_pass == NULL ||
        target_view == NULL || target_depth == NULL) {
        return false;
    }

    wgpuRenderPassEncoderEnd(s_pass);
    wgpuRenderPassEncoderRelease(s_pass);
    s_pass = NULL;

    if (s_vbuf != NULL && s_vbuf_shadow != NULL && s_vbuf_off > 0) {
        wgpuQueueWriteBuffer(s_queue, s_vbuf, 0, s_vbuf_shadow, s_vbuf_off);
    }

    WGPUCommandBuffer cmd = WGPU_FAULT_CREATE(
        PARTIAL_FINISH, wgpuCommandEncoderFinish(s_encoder, NULL));
    if (cmd == NULL) {
        wgpuCommandEncoderRelease(s_encoder);
        s_encoder = NULL;
        s_frame_open = false;
        fprintf(stderr, "[webgpu] partial readback submit creation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not submit a diagnostic frame. Reload "
            "the page to continue from the last persisted save.");
        return false;
    }
    if (!wgpu_submit_commands(s_queue, 1, &cmd)) {
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(s_encoder);
        s_encoder = NULL;
        s_frame_open = false;
        return false;
    }
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(s_encoder);

    s_encoder = WGPU_FAULT_CREATE(
        PARTIAL_ENCODER, wgpuDeviceCreateCommandEncoder(s_device, NULL));
    if (s_encoder == NULL) {
        s_frame_open = false;
        fprintf(stderr, "[webgpu] partial readback encoder creation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not resume after a diagnostic readback. "
            "Reload the page to continue from the last persisted save.");
        return false;
    }

    WGPURenderPassColorAttachment att = {0};
    att.view = target_view;
    att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    att.loadOp = WGPULoadOp_Load;
    att.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDepthStencilAttachment depth = {0};
    depth.view = target_depth;
    depth.depthLoadOp = WGPULoadOp_Load;
    depth.depthStoreOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp = {0};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;
    rp.depthStencilAttachment = &depth;
    s_pass = WGPU_FAULT_CREATE(
        PARTIAL_PASS, wgpuCommandEncoderBeginRenderPass(s_encoder, &rp));
    if (s_pass == NULL) {
        wgpuCommandEncoderRelease(s_encoder);
        s_encoder = NULL;
        s_frame_open = false;
        fprintf(stderr, "[webgpu] partial readback resume pass failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not resume after a diagnostic readback. "
            "Reload the page to continue from the last persisted save.");
        return false;
    }

    /* A new pass inherits no dynamic state.  gfx_pc.c re-applies the current
     * viewport/scissor/pipeline before the next draw. */
    wgpu_reset_pass_dynamic_state();
    return true;
}

/* Read back the last-rendered offscreen scene as GL-convention
 * bottom-left RGB (so platformSaveScreenshot + the parity/oracle tooling work on
 * WebGPU identically to GL/Metal). Copies the whole BGRA8 scene into a mappable
 * buffer, then extracts the requested rect with a vertical flip + BGRA->RGB.
 * Synchronous (submit + poll-map) — only screenshot/parity/diagnostic paths call
 * it.  During an open frame, partially submit first so draw-boundary probes see
 * the live scene rather than s_present_target_tex from the previous frame. */
static bool wgpu_read_framebuffer_rgb(int x, int y, int width, int height, uint8_t *rgb_out) {
    /* PERF-008: this session performs readbacks — pin every subsequent frame to the
     * offscreen present path (which retains a readable s_present_target_tex). Sticky,
     * so it protects any readback caller whose arming signal wgpu_readback_possible()
     * did not pre-enumerate; the fire during gfx_run_dl trips before this frame's own
     * end_frame, so even the current frame presents offscreen. */
    s_readback_latched = true;
    bool live_scene = s_frame_open;
    bool live_output_overlay = live_scene && s_output_overlay_active;
    if (live_scene && !wgpu_submit_live_scene_for_readback()) {
        return false;
    }
    /* During a frame, the freshly submitted raw scene is authoritative.  After
     * end_frame, preserve AUDIT-0003 behavior and read the post-FX/minimap target. */
    WGPUTexture rb_tex = live_scene
        ? (live_output_overlay ? s_resolve_tex : s_scene_tex)
        : (s_present_target_tex ? s_present_target_tex : s_scene_tex);
    if (platform_frame_dump_due() &&
        getenv("MDKR_FRAME_DUMP_TRACE") != NULL) {
        fprintf(stderr,
                "[FRAME-DUMP] WebGPU readback frame=%d live=%d overlay=%d "
                "submitted=%llu completed=%llu\n",
                g_frameCounter, live_scene ? 1 : 0,
                live_output_overlay ? 1 : 0,
                (unsigned long long)s_gpu_frame_submissions,
                (unsigned long long)s_gpu_frame_completions);
    }
    uint32_t rb_w = live_output_overlay ? s_resolve_w : s_scene_w;
    uint32_t rb_h = live_output_overlay ? s_resolve_h : s_scene_h;

    /*
     * IQ-10. With Video.RenderScale > 1 the live scene is at RENDER resolution,
     * but every caller here -- the frame dumper, the oracle harness, the
     * screenshot path -- asks for OUTPUT-sized pixels. Handing them the scaled
     * scene would not error; it would quietly reinterpret every fidelity score
     * in the project as a score against a supersampled image. So resolve first
     * and read the resolved texture.
     *
     * The post-end_frame path needs nothing: s_present_target_tex is already the
     * resolve texture on a supersampled frame.
     */
    if (live_scene && !live_output_overlay && s_resolve_view != NULL &&
        (s_scene_w != s_resolve_w || s_scene_h != s_resolve_h)) {
        WGPUCommandEncoder renc = WGPU_FAULT_CREATE(
            READBACK_RESOLVE_ENCODER,
            wgpuDeviceCreateCommandEncoder(s_device, NULL));
        if (renc == NULL) {
            return false;
        }
        bool ok = wgpu_run_resolve(renc, s_scene_view, s_scene_w, s_scene_h,
                                   s_resolve_w, s_resolve_h);
        WGPUCommandBuffer rcmd = ok
            ? WGPU_FAULT_CREATE(
                READBACK_RESOLVE_FINISH,
                wgpuCommandEncoderFinish(renc, NULL))
            : NULL;
        if (rcmd != NULL) {
            if (!wgpu_submit_commands(s_queue, 1, &rcmd)) {
                ok = false;
            }
            wgpuCommandBufferRelease(rcmd);
        }
        wgpuCommandEncoderRelease(renc);
        if (!ok || rcmd == NULL) {
            return false;
        }
        rb_tex = s_resolve_tex;
        rb_w = s_resolve_w;
        rb_h = s_resolve_h;
    } else if (!live_scene && rb_tex == s_resolve_tex) {
        rb_w = s_resolve_w;
        rb_h = s_resolve_h;
    }
    if (!s_ready || rb_tex == NULL || rgb_out == NULL ||
        x < 0 || y < 0 || width <= 0 || height <= 0 ||
        rb_w == 0 || rb_h == 0 ||
        (uint64_t)(uint32_t)x + (uint64_t)(uint32_t)width > rb_w ||
        (uint64_t)(uint32_t)y + (uint64_t)(uint32_t)height > rb_h) {
        return false;
    }
    uint32_t bpr = ((rb_w * 4u + 255u) / 256u) * 256u;   /* 256-align */
    size_t buf_size = (size_t)bpr * rb_h;
    WGPUBufferDescriptor bd = {0};
    bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    bd.size = buf_size;
    WGPUBuffer buf = WGPU_FAULT_CREATE(
        READBACK_BUFFER, wgpuDeviceCreateBuffer(s_device, &bd));
    if (buf == NULL) {
        return false;
    }

    WGPUCommandEncoder enc = WGPU_FAULT_CREATE(
        READBACK_ENCODER, wgpuDeviceCreateCommandEncoder(s_device, NULL));
    if (enc == NULL) {
        wgpuBufferRelease(buf);
        return false;
    }
    WGPUTexelCopyTextureInfo src = {0};
    src.texture = rb_tex; src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst = {0};
    dst.buffer = buf;
    dst.layout.bytesPerRow = bpr;
    dst.layout.rowsPerImage = rb_h;
    WGPUExtent3D ext = { rb_w, rb_h, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
    WGPUCommandBuffer cmd = WGPU_FAULT_CREATE(
        READBACK_FINISH, wgpuCommandEncoderFinish(enc, NULL));
    if (cmd == NULL) {
        wgpuCommandEncoderRelease(enc);
        wgpuBufferRelease(buf);
        return false;
    }
    if (!wgpu_submit_commands(s_queue, 1, &cmd)) {
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(enc);
        wgpuBufferRelease(buf);
        return false;
    }
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);

    /* WEB-026: static so a timed-out map's late callback lands on live storage,
     * not this frame's dead stack. Synchronous single-threaded call; reset here. */
    static WgpuMapReq mr;
    mr = (WgpuMapReq){0};
    WGPUBufferMapCallbackInfo ci = {0};
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = on_map;
    ci.userdata1 = &mr;
    if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_READBACK_MAP)) {
        mr.done = 1;
        mr.status = WGPUMapAsyncStatus_Error;
    } else {
        wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, buf_size, ci);
    }
    /* WEB-004: pass s_instance (not NULL) so the browser pump can drive
     * wgpuInstanceProcessEvents — the mapAsync callback ONLY fires during
     * ProcessEvents, so a NULL instance froze the tab for minutes on web. On
     * native the WAIT macro prefers the (non-NULL) device and calls
     * wgpuDevicePoll exactly as before — byte-identical. */
    WGPU_PIPELINE_CALLBACK_OWNER_WAIT(
        mr.done, s_instance, s_device, 100000);
    if (!mr.done || mr.status != WGPUMapAsyncStatus_Success) {
        wgpuBufferRelease(buf);
        return false;
    }
    const uint8_t *px = (const uint8_t *)wgpuBufferGetConstMappedRange(buf, 0, buf_size);
    if (px == NULL) {
        wgpuBufferUnmap(buf);
        wgpuBufferRelease(buf);
        return false;
    }
    /* rgb_out is width*height RGB, bottom-left origin (GL convention). The scene
     * is top-left, so scene row (H-1 - (y+row)) maps to output row `row`. */
    for (int row = 0; row < height; row++) {
        int scene_y = (int)rb_h - 1 - (y + row);
        uint8_t *out = rgb_out + (size_t)row * width * 3;
        if (scene_y < 0 || scene_y >= (int)rb_h) {
            memset(out, 0, (size_t)width * 3);
            continue;
        }
        const uint8_t *srow = px + (size_t)scene_y * bpr;
        const bool bgra = wgpu_format_is_bgra(s_surface_format);
        for (int col = 0; col < width; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= (int)rb_w) {
                out[col*3] = out[col*3+1] = out[col*3+2] = 0;
                continue;
            }
            const uint8_t *p = srow + (size_t)sx * 4;   /* BGRA8 (or RGBA8) */
            out[col*3+0] = bgra ? p[2] : p[0];   /* R */
            out[col*3+1] = p[1];                 /* G */
            out[col*3+2] = bgra ? p[0] : p[2];   /* B */
        }
    }
    wgpuBufferUnmap(buf);
    wgpuBufferRelease(buf);
    return true;
}

/* ------------------------------------------------------------------------
 * Optional: draw_modern_mesh (scene decor, G_MODERNMESH).
 *
 * A full-fidelity mesh (float32 pos/nrm/uv + rgba8, u32 indices, RGBA8 texture)
 * drawn into the live scene pass, transformed by the interpreter's MP matrix
 * with the N64 fog curve. Ports gfx_metal.mm's decor shader + cache. Renders
 * scene decoration, which is a no-op on the diagnostic GL backend (AUDIT-0001); needs
 * Video.SceneDecor=1. Per-mesh GPU resources are cached by mesh_id (ids are
 * monotonic/never reused, so a full evict at capacity is safe).
 * ---------------------------------------------------------------------- */
static const char *kModernWGSL =
    "struct DVin { @location(0) pos : vec3<f32>, @location(1) nrm : vec3<f32>, @location(2) uv : vec2<f32>, @location(3) col : vec4<f32> };\n"
    "struct DU { mvp : mat4x4<f32>, fog : vec4<f32>, fogMul : f32, fogOffset : f32, fogOn : f32, farClamp : f32 };\n"
    "@group(0) @binding(0) var<uniform> u : DU;\n"
    "@group(0) @binding(1) var dtex : texture_2d<f32>;\n"
    "@group(0) @binding(2) var dsmp : sampler;\n"
    "struct DOut { @builtin(position) position : vec4<f32>, @location(0) uv : vec2<f32>, @location(1) col : vec4<f32>, @location(2) fogA : f32 };\n"
    "@vertex fn vs_main(in : DVin) -> DOut {\n"
    "  var clip = u.mvp * vec4<f32>(in.pos, 1.0);\n"
    "  var fogA = 0.0;\n"
    "  if (u.fogOn > 0.5) {\n"
    "    var ww = clip.w;\n"
    "    if (abs(ww) < 0.001) { ww = 0.001; }\n"
    "    let winv = 1.0 / ww;\n"
    "    let coord = select(clip.z * winv, clip.z * 32767.0, winv < 0.0);\n"
    "    fogA = clamp(coord * u.fogMul + u.fogOffset, 0.0, 255.0) / 255.0;\n"
    "  }\n"
    "  if (u.farClamp > 0.5 && clip.z > clip.w) { clip.z = clip.w; }\n"
    "  var o : DOut;\n"
    "  clip.z = (clip.z + clip.w) * 0.5;\n"   /* GL-clip -> WebGPU 0..1, like Metal */
    "  o.position = clip; o.uv = in.uv; o.col = in.col; o.fogA = fogA;\n"
    "  return o;\n}\n"
    "fn decorShade(o : DOut, t : vec4<f32>) -> vec4<f32> {\n"
    "  var c = t.rgb * o.col.rgb;\n"
    "  c = mix(c, vec3<f32>(0.88, 0.91, 0.96), o.col.a);\n"   /* snow cover in vertex alpha */
    "  c = mix(c, u.fog.rgb, o.fogA);\n"
    "  return vec4<f32>(c, t.a);\n}\n"
    "@fragment fn fs_opaque(in : DOut) -> @location(0) vec4<f32> {\n"
    "  let c = decorShade(in, textureSample(dtex, dsmp, in.uv));\n"
    "  return vec4<f32>(c.rgb, 1.0);\n}\n"
    "@fragment fn fs_cutout(in : DOut) -> @location(0) vec4<f32> {\n"
    "  let t = textureSample(dtex, dsmp, in.uv);\n"
    "  if (t.a < 0.45) { discard; }\n"
    "  let c = decorShade(in, t);\n"
    "  return vec4<f32>(c.rgb, 1.0);\n}\n";

static WGPUShaderModule    s_modern_mod = NULL;
static WGPUBindGroupLayout s_modern_bgl = NULL;
static WGPUPipelineLayout  s_modern_pl = NULL;
static WGPURenderPipeline  s_modern_pipe[2] = {NULL, NULL};   /* [cutout] */
static WGPUSampler         s_modern_sampler = NULL;

struct WgpuModernEntry {
    uint32_t mesh_id;
    WGPUBuffer vbuf, ibuf;
    WGPUTexture tex;
    WGPUTextureView view;
    uint32_t idx_count;
    /* WEB-052: bind group cached across frames (references the shared dynamic-
     * offset UBO ring + this mesh's texture/sampler). bg_gen stamps the ring
     * buffer generation it was built against, so a ring grow (buffer recreate)
     * forces a rebuild. */
    WGPUBindGroup bg;
    uint32_t bg_gen;
};
static struct WgpuModernEntry s_modern_cache[64];
static int s_modern_count = 0;

static WGPURenderPipeline wgpu_modern_pipe(int cutout) {
    if (s_modern_pipe[cutout] != NULL) {
        return s_modern_pipe[cutout];
    }
    if (s_modern_mod == NULL) {
        WGPUShaderModule new_mod = NULL;
        WGPUBindGroupLayout new_bgl = NULL;
        WGPUPipelineLayout new_layout = NULL;
        WGPUShaderSourceWGSL src = {0};
        src.chain.sType = WGPUSType_ShaderSourceWGSL;
        src.code = wgpu_sv(kModernWGSL);
        WGPUShaderModuleDescriptor smd = {0};
        smd.nextInChain = (WGPUChainedStruct *)&src;
        new_mod = WGPU_FAULT_CREATE(
            MODERN_MODULE, wgpuDeviceCreateShaderModule(s_device, &smd));
        if (new_mod == NULL) goto init_fail;

        WGPUBindGroupLayoutEntry e[3] = {0};
        e[0].binding = 0; e[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        e[0].buffer.type = WGPUBufferBindingType_Uniform; e[0].buffer.minBindingSize = 96;
        e[0].buffer.hasDynamicOffset = true;   /* WEB-052: ring slot via dynamic offset */
        e[1].binding = 1; e[1].visibility = WGPUShaderStage_Fragment;
        e[1].texture.sampleType = WGPUTextureSampleType_Float; e[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        e[2].binding = 2; e[2].visibility = WGPUShaderStage_Fragment;
        e[2].sampler.type = WGPUSamplerBindingType_Filtering;
        WGPUBindGroupLayoutDescriptor bgld = {0};
        bgld.entryCount = 3; bgld.entries = e;
        new_bgl = WGPU_FAULT_CREATE(
            MODERN_BGL, wgpuDeviceCreateBindGroupLayout(s_device, &bgld));
        if (new_bgl == NULL) goto init_fail;
        WGPUPipelineLayoutDescriptor pld = {0};
        pld.bindGroupLayoutCount = 1; pld.bindGroupLayouts = &new_bgl;
        new_layout = WGPU_FAULT_CREATE(
            MODERN_LAYOUT,
            wgpuDeviceCreatePipelineLayout(s_device, &pld));
        if (new_layout == NULL) goto init_fail;
        s_modern_mod = new_mod;
        s_modern_bgl = new_bgl;
        s_modern_pl = new_layout;
        goto init_done;

init_fail:
        if (new_layout != NULL) wgpuPipelineLayoutRelease(new_layout);
        if (new_bgl != NULL) wgpuBindGroupLayoutRelease(new_bgl);
        if (new_mod != NULL) wgpuShaderModuleRelease(new_mod);
        fprintf(stderr, "[webgpu] transactional modern-pipeline setup failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not allocate its modern-mesh pipeline. "
            "Reload the page to continue from the last persisted save.");
        return NULL;
init_done:
        ;
    }

    WGPUVertexAttribute a[4] = {0};
    a[0].format = WGPUVertexFormat_Float32x3; a[0].offset = 0;  a[0].shaderLocation = 0;
    a[1].format = WGPUVertexFormat_Float32x3; a[1].offset = 12; a[1].shaderLocation = 1;
    a[2].format = WGPUVertexFormat_Float32x2; a[2].offset = 24; a[2].shaderLocation = 2;
    a[3].format = WGPUVertexFormat_Unorm8x4;  a[3].offset = 32; a[3].shaderLocation = 3;
    WGPUVertexBufferLayout vbl = {0};
    vbl.stepMode = WGPUVertexStepMode_Vertex;
    vbl.arrayStride = 36;
    vbl.attributeCount = 4;
    vbl.attributes = a;

    WGPUColorTargetState color = {0};
    color.format = s_surface_format;
    color.writeMask = WGPUColorWriteMask_All;   /* opaque (frag outputs a=1) */
    WGPUFragmentState fs = {0};
    fs.module = s_modern_mod;
    fs.entryPoint = wgpu_sv(cutout ? "fs_cutout" : "fs_opaque");
    fs.targetCount = 1; fs.targets = &color;

    WGPUDepthStencilState ds = {0};
    ds.format = WGPU_DEPTH_FORMAT;
    ds.depthCompare = WGPUCompareFunction_LessEqual;
    ds.depthWriteEnabled = WGPUOptionalBool_True;
    ds.stencilFront.compare = WGPUCompareFunction_Always;
    ds.stencilFront.failOp = WGPUStencilOperation_Keep;
    ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
    ds.stencilFront.passOp = WGPUStencilOperation_Keep;
    ds.stencilBack = ds.stencilFront;

    WGPURenderPipelineDescriptor pd = {0};
    pd.layout = s_modern_pl;
    pd.vertex.module = s_modern_mod; pd.vertex.entryPoint = wgpu_sv("vs_main");
    pd.vertex.bufferCount = 1; pd.vertex.buffers = &vbl;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;   /* cutout cards are two-sided */
    pd.primitive.unclippedDepth =
        s_unclipped_depth_supported ? WGPU_TRUE : WGPU_FALSE;
    pd.depthStencil = &ds;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    s_modern_pipe[cutout] = WGPU_FAULT_CREATE(
        MODERN_PIPELINE, wgpuDeviceCreateRenderPipeline(s_device, &pd));
    if (s_modern_pipe[cutout] == NULL) {
        fprintf(stderr, "[webgpu] modern render-pipeline creation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not compile its modern-mesh pipeline. "
            "Reload the page to continue from the last persisted save.");
    }
    return s_modern_pipe[cutout];
}

static struct WgpuModernEntry *wgpu_modern_resources(struct GfxModernMesh *mesh) {
    for (int i = 0; i < s_modern_count; i++) {
        if (s_modern_cache[i].mesh_id == mesh->mesh_id) return &s_modern_cache[i];
    }
    if (s_modern_count >= (int)(sizeof(s_modern_cache) / sizeof(s_modern_cache[0]))) {
        /* Level churn: ids never repeat, so releasing everything is safe. */
        for (int i = 0; i < s_modern_count; i++) {
            if (s_modern_cache[i].bg)   wgpuBindGroupRelease(s_modern_cache[i].bg);
            if (s_modern_cache[i].view) wgpuTextureViewRelease(s_modern_cache[i].view);
            if (s_modern_cache[i].tex)  wgpuTextureRelease(s_modern_cache[i].tex);
            if (s_modern_cache[i].vbuf) wgpuBufferRelease(s_modern_cache[i].vbuf);
            if (s_modern_cache[i].ibuf) wgpuBufferRelease(s_modern_cache[i].ibuf);
        }
        s_modern_count = 0;
    }

    uint64_t vbytes64 = (uint64_t)mesh->vtx_count * 36u;
    uint64_t ibytes64 = (uint64_t)mesh->idx_count * 4u;
    if (vbytes64 == 0 || ibytes64 == 0 ||
        vbytes64 > UINT32_MAX || ibytes64 > UINT32_MAX ||
        mesh->tex_w <= 0 || mesh->tex_h <= 0 ||
        (uint32_t)mesh->tex_w > s_max_tex_dim ||
        (uint32_t)mesh->tex_h > s_max_tex_dim) {
        fprintf(stderr, "[webgpu] invalid modern-mesh resource dimensions\n");
        wgpu_runtime_fatal(
            "A modern mesh exceeds the graphics backend's resource limits. "
            "Reload the page to continue from the last persisted save.");
        return NULL;
    }
    uint32_t vbytes = (uint32_t)vbytes64;
    uint32_t ibytes = (uint32_t)ibytes64;
    WGPUBufferDescriptor vd = {0};
    vd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst; vd.size = vbytes;
    WGPUBuffer vb = WGPU_FAULT_CREATE(
        MODERN_VERTEX_BUFFER, wgpuDeviceCreateBuffer(s_device, &vd));
    WGPUBufferDescriptor id = {0};
    id.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst; id.size = ibytes;
    WGPUBuffer ib = WGPU_FAULT_CREATE(
        MODERN_INDEX_BUFFER, wgpuDeviceCreateBuffer(s_device, &id));
    if (vb == NULL || ib == NULL) {
        if (vb) wgpuBufferRelease(vb);
        if (ib) wgpuBufferRelease(ib);
        fprintf(stderr, "[webgpu] modern-mesh buffer allocation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not allocate a modern mesh. Reload "
            "the page to continue from the last persisted save.");
        return NULL;
    }
    wgpuQueueWriteBuffer(s_queue, vb, 0, mesh->vtx, vbytes);
    wgpuQueueWriteBuffer(s_queue, ib, 0, mesh->idx, ibytes);

    WGPUTextureDescriptor td = {0};
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)mesh->tex_w; td.size.height = (uint32_t)mesh->tex_h; td.size.depthOrArrayLayers = 1;
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1; td.sampleCount = 1;   /* single-level (no mip gen) */
    WGPUTexture tex = WGPU_FAULT_CREATE(
        MODERN_TEXTURE, wgpuDeviceCreateTexture(s_device, &td));
    if (tex == NULL) {
        wgpuBufferRelease(vb);
        wgpuBufferRelease(ib);
        fprintf(stderr, "[webgpu] modern-mesh texture allocation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not allocate a modern-mesh texture. "
            "Reload the page to continue from the last persisted save.");
        return NULL;
    }
    WGPUTexelCopyTextureInfo dst = {0};
    dst.texture = tex; dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout lay = {0};
    lay.bytesPerRow = (uint32_t)mesh->tex_w * 4u; lay.rowsPerImage = (uint32_t)mesh->tex_h;
    WGPUExtent3D ext = { (uint32_t)mesh->tex_w, (uint32_t)mesh->tex_h, 1 };
    wgpuQueueWriteTexture(s_queue, &dst, mesh->tex_rgba, (size_t)mesh->tex_w * mesh->tex_h * 4u, &lay, &ext);

    WGPUTextureView view = WGPU_FAULT_CREATE(
        MODERN_VIEW, wgpuTextureCreateView(tex, NULL));
    if (view == NULL) {
        wgpuTextureRelease(tex);
        wgpuBufferRelease(vb);
        wgpuBufferRelease(ib);
        fprintf(stderr, "[webgpu] modern-mesh texture-view allocation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not allocate a modern-mesh texture "
            "view. Reload the page to continue from the last persisted save.");
        return NULL;
    }
    struct WgpuModernEntry *e = &s_modern_cache[s_modern_count++];
    e->mesh_id = mesh->mesh_id;
    e->vbuf = vb; e->ibuf = ib; e->tex = tex;
    e->view = view;
    e->idx_count = mesh->idx_count;
    e->bg = NULL; e->bg_gen = 0;   /* WEB-052: built lazily on first draw */
    return e;
}

/* WEB-052: ensure the ring UBO holds at least `need_slots` 256-byte slots. Grows
 * (recreates) the buffer at need; a grow bumps the generation so cached per-mesh
 * bind groups (which reference the old buffer) rebuild against the new one. Never
 * shrinks the buffer within a frame — a slot already written this frame stays
 * referenced by an earlier draw's dynamic-offset bind. */
static bool wgpu_modern_ubo_reserve(int need_slots) {
    if (s_modern_ubo != NULL && need_slots <= s_modern_ubo_cap) {
        return true;
    }
    int ncap = s_modern_ubo_cap ? s_modern_ubo_cap : WGPU_MODERN_UBO_INIT;
    while (ncap < need_slots) {
        ncap *= 2;
    }
    WGPUBufferDescriptor bd = {0};
    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    bd.size = (uint64_t)ncap * WGPU_MODERN_UBO_ALIGN;
    WGPUBuffer nb = WGPU_FAULT_CREATE(
        MODERN_UNIFORM, wgpuDeviceCreateBuffer(s_device, &bd));
    if (nb == NULL) {
        fprintf(stderr, "[webgpu] modern uniform-ring allocation failed\n");
        wgpu_runtime_fatal(
            "The graphics backend could not allocate its modern-mesh uniform "
            "ring. Reload the page to continue from the last persisted save.");
        return false;
    }
    /* Release the app-side handle to the old buffer: any bind group/encoder from
     * earlier this frame retains its own strong ref until submit, so this is safe. */
    if (s_modern_ubo != NULL) {
        wgpuBufferRelease(s_modern_ubo);
    }
    s_modern_ubo = nb;
    s_modern_ubo_cap = ncap;
    s_modern_ubo_gen++;
    return true;
}

static void wgpu_draw_modern_mesh(struct GfxModernMesh *mesh, const float mvp[4][4],
                                  const float fog_color[3], float fog_mul,
                                  float fog_offset, int fog_enabled) {
    if (!s_ready || !s_frame_open || s_pass == NULL || mesh == NULL ||
        mesh->vtx == NULL || mesh->idx == NULL || mesh->tex_rgba == NULL ||
        mesh->tex_w <= 0 || mesh->tex_h <= 0) {
        return;
    }
    int cutout = mesh->cutout ? 1 : 0;
    WGPURenderPipeline pipe = wgpu_modern_pipe(cutout);
    if (pipe == NULL) return;
    struct WgpuModernEntry *res = wgpu_modern_resources(mesh);
    if (res == NULL) return;

    if (s_modern_sampler == NULL) {
        WGPUSamplerDescriptor sd = {0};
        sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_Repeat;
        sd.magFilter = sd.minFilter = WGPUFilterMode_Linear;
        sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;   /* single-level */
        sd.maxAnisotropy = 1;
        s_modern_sampler = WGPU_FAULT_CREATE(
            MODERN_SAMPLER, wgpuDeviceCreateSampler(s_device, &sd));
        if (s_modern_sampler == NULL) {
            fprintf(stderr, "[webgpu] modern-mesh sampler creation failed\n");
            wgpu_runtime_fatal(
                "The graphics backend could not allocate its modern-mesh "
                "sampler. Reload the page to continue from the last persisted "
                "save.");
            return;
        }
    }

    /* Uniform (96 bytes): row-major MP memcpy'd into the mat4x4 (columns == MP
     * rows, so u.mvp * v == the CPU row-vector T&L), fog params. */
    float u[24];
    memset(u, 0, sizeof(u));
    memcpy(u, mvp, 16 * sizeof(float));
    u[16] = fog_color[0]; u[17] = fog_color[1]; u[18] = fog_color[2]; u[19] = 0.0f;
    u[20] = fog_mul;
    u[21] = fog_offset;
    u[22] = fog_enabled ? 1.0f : 0.0f;
    u[23] = s_unclipped_depth_supported ? 0.0f : 1.0f;

    /* WEB-052: claim a FRESH ring slot (never a slot already written this frame),
     * write the uniform there, and bind it via a dynamic offset — instead of
     * allocating a one-off UBO + bind group per draw. */
    if (!wgpu_modern_ubo_reserve(s_modern_ubo_used + 1)) {
        return;
    }
    int slot = s_modern_ubo_used++;
    uint32_t dyn_off = (uint32_t)slot * WGPU_MODERN_UBO_ALIGN;
    wgpuQueueWriteBuffer(s_queue, s_modern_ubo, dyn_off, u, sizeof(u));

    /* Per-mesh bind group, cached across frames. Rebuild it only when absent or
     * built against a stale ring buffer (a grow bumped s_modern_ubo_gen). Its
     * uniform binding covers ONE slot at offset 0; the actual slot is selected by
     * the dynamic offset passed to SetBindGroup below. */
    if (res->bg == NULL || res->bg_gen != s_modern_ubo_gen) {
        if (res->bg != NULL) { wgpuBindGroupRelease(res->bg); res->bg = NULL; }
        WGPUBindGroupEntry be[3] = {0};
        be[0].binding = 0; be[0].buffer = s_modern_ubo; be[0].offset = 0; be[0].size = sizeof(u);
        be[1].binding = 1; be[1].textureView = res->view;
        be[2].binding = 2; be[2].sampler = s_modern_sampler;
        WGPUBindGroupDescriptor bgd = {0};
        bgd.layout = s_modern_bgl; bgd.entryCount = 3; bgd.entries = be;
        res->bg = WGPU_FAULT_CREATE(
            MODERN_BIND_GROUP,
            wgpuDeviceCreateBindGroup(s_device, &bgd));
        res->bg_gen = s_modern_ubo_gen;
        if (res->bg == NULL) {
            fprintf(stderr, "[webgpu] modern-mesh bind-group creation failed\n");
            wgpu_runtime_fatal(
                "The graphics backend could not bind a modern mesh. Reload the "
                "page to continue from the last persisted save.");
            return;
        }
    }

    wgpuRenderPassEncoderSetPipeline(s_pass, pipe);
    wgpuRenderPassEncoderSetBindGroup(s_pass, 0, res->bg, 1, &dyn_off);
    /* PERF-014: this draw wrote s_pass's pipeline/bind group directly, bypassing
     * wgpu_draw_triangles' dedup while sharing the same scene pass. Keep the
     * trackers honest so the next wgpu_draw_triangles cannot wrongly skip a needed
     * re-bind: `pipe` IS now the bound pipeline (record it, so a following triangle
     * draw with the same pipeline can still skip); res->bg was bound WITH a dynamic
     * offset, so force the next triangle draw to re-issue its group(0) bind (NULL
     * sentinel) rather than record a handle a different dynamic offset could alias. */
    s_pipe_applied = pipe;
    s_bg_applied   = NULL;
    wgpuRenderPassEncoderSetVertexBuffer(s_pass, 0, res->vbuf, 0, mesh->vtx_count * 36u);
    wgpuRenderPassEncoderSetIndexBuffer(s_pass, res->ibuf, WGPUIndexFormat_Uint32, 0, mesh->idx_count * 4u);
    wgpuRenderPassEncoderDrawIndexed(s_pass, res->idx_count, 1, 0, 0, 0);
    /* res->bg is owned by the mesh cache; the ring UBO persists — nothing to
     * release here (the encoder retains referenced resources until submit). */
}

/* WebGPU clip space is 0..1 (like Metal/D3D, unlike GL's -1..1). The frontend
 * also queries gfx_webgpu_unclipped_depth_supported() and clamps homogeneous z
 * in software when the optional DepthClipControl feature is absent. */
static bool wgpu_z_is_from_0_to_1(void) { return true; }

/* ------------------------------------------------------------------------
 * Non-vtable couplings (called directly from gfx_pc.c, backend-aware)
 * ---------------------------------------------------------------------- */
void gfx_webgpu_set_clear_color(float r, float g, float b) {
    s_clear_r = (double)r;
    s_clear_g = (double)g;
    s_clear_b = (double)b;
}

int gfx_webgpu_max_offscreen_dim(void) {
    /* WEB-015: the device's granted maxTextureDimension2D (adapter max, up from
     * the 8192 WebGPU default), or the 8192 floor when bring-up was skipped. */
    return (int)s_max_tex_dim;
}

bool gfx_webgpu_unclipped_depth_supported(void) {
    /* Whether the 3D pipelines were built with unclippedDepth (i.e.
     * DepthClipControl was granted, so the GPU depth-clamps like GL/Metal).
     * gfx_pc_dkr.c supplies the equivalent homogeneous-z clamp when false. */
    return s_unclipped_depth_supported;
}

bool gfx_webgpu_get_output_size(int *width, int *height) {
    if (width == NULL || height == NULL ||
        s_resolve_w == 0 || s_resolve_h == 0 ||
        s_resolve_tex == NULL) {
        return false;
    }
    *width = (int)s_resolve_w;
    *height = (int)s_resolve_h;
    return true;
}

bool gfx_webgpu_runtime_recovery_pending(void) {
#ifdef __EMSCRIPTEN__
    return false;
#else
    (void)wgpu_consume_callback_failure();
    return s_runtime_status == GFX_RENDERING_FATAL &&
        (s_owns_device ||
         (s_callback_recovery_pending && platformHasHostWebGpuRecovery()));
#endif
}

/*
 * Release every application-held reference owned by the current device before
 * destroying it. wgpuDeviceDestroy invalidates children, but the C handles are
 * still reference-counted; merely zeroing them leaks the host-side wrappers and
 * any transitive references they retain. Browser async-pipeline callbacks carry
 * a generation token and reject stale program pointers before this frees them.
 */
static void wgpu_release_device_objects(void) {
    if (s_overlay_pass != NULL) {
        wgpuRenderPassEncoderRelease(s_overlay_pass);
    }
    if (s_pass != NULL) {
        wgpuRenderPassEncoderRelease(s_pass);
    }
    if (s_encoder != NULL) {
        wgpuCommandEncoderRelease(s_encoder);
    }
    /* No pass remains capable of consuming cached state. Clear its raw-handle
     * memo before releasing any cache member so recovery starts from the same
     * ownership invariant as an ordinary cache eviction. */
    wgpu_reset_pass_dynamic_state();

    wgpu_release_shadow_resources();
    if (s_post_bg != NULL) wgpuBindGroupRelease(s_post_bg);
    if (s_mm_bg != NULL) wgpuBindGroupRelease(s_mm_bg);
    for (int i = 0; i < WGPU_BG_CACHE; i++) {
        if (s_bg_cache_tab[i].bg != NULL) {
            wgpuBindGroupRelease(s_bg_cache_tab[i].bg);
        }
    }
    for (int i = 0; i < s_modern_count; i++) {
        if (s_modern_cache[i].bg != NULL) {
            wgpuBindGroupRelease(s_modern_cache[i].bg);
        }
    }

    for (size_t i = 0; i < s_shader_count; i++) {
        struct ShaderProgram *prg = s_shaders[i];
        if (prg == NULL) continue;
        for (int j = 0; j < WGPU_PIPE_CACHE; j++) {
            if (prg->pipes[j].pipe != NULL) {
                wgpuRenderPipelineRelease(prg->pipes[j].pipe);
            }
        }
        if (prg->playout != NULL) wgpuPipelineLayoutRelease(prg->playout);
        if (prg->bgl != NULL) wgpuBindGroupLayoutRelease(prg->bgl);
        if (prg->module != NULL) wgpuShaderModuleRelease(prg->module);
    }
    if (s_resolve_pipe != NULL) wgpuRenderPipelineRelease(s_resolve_pipe);
    if (s_post_pipe != NULL) wgpuRenderPipelineRelease(s_post_pipe);
    if (s_mm_pipe != NULL) wgpuRenderPipelineRelease(s_mm_pipe);
    for (int i = 0; i < 2; i++) {
        if (s_modern_pipe[i] != NULL) {
            wgpuRenderPipelineRelease(s_modern_pipe[i]);
        }
    }

    if (s_resolve_bgl != NULL) wgpuBindGroupLayoutRelease(s_resolve_bgl);
    if (s_post_bgl != NULL) wgpuBindGroupLayoutRelease(s_post_bgl);
    if (s_mm_bgl != NULL) wgpuBindGroupLayoutRelease(s_mm_bgl);
    if (s_modern_bgl != NULL) wgpuBindGroupLayoutRelease(s_modern_bgl);
    if (s_modern_pl != NULL) wgpuPipelineLayoutRelease(s_modern_pl);
    if (s_modern_mod != NULL) wgpuShaderModuleRelease(s_modern_mod);

    for (int i = 0; i < s_modern_count; i++) {
        if (s_modern_cache[i].view != NULL) {
            wgpuTextureViewRelease(s_modern_cache[i].view);
        }
        if (s_modern_cache[i].tex != NULL) {
            wgpuTextureRelease(s_modern_cache[i].tex);
        }
        if (s_modern_cache[i].vbuf != NULL) {
            wgpuBufferRelease(s_modern_cache[i].vbuf);
        }
        if (s_modern_cache[i].ibuf != NULL) {
            wgpuBufferRelease(s_modern_cache[i].ibuf);
        }
    }
    if (s_tex != NULL) {
        for (size_t i = 0; i < s_tex_hi; i++) {
            if (s_tex[i].view != NULL) wgpuTextureViewRelease(s_tex[i].view);
            if (s_tex[i].tex != NULL) wgpuTextureRelease(s_tex[i].tex);
        }
    }

    if (s_resolve_view != NULL) wgpuTextureViewRelease(s_resolve_view);
    if (s_output_depth_view != NULL) wgpuTextureViewRelease(s_output_depth_view);
    if (s_scene_view != NULL) wgpuTextureViewRelease(s_scene_view);
    if (s_depth_view != NULL) wgpuTextureViewRelease(s_depth_view);
    if (s_post_view != NULL) wgpuTextureViewRelease(s_post_view);
    if (s_snap_view != NULL) wgpuTextureViewRelease(s_snap_view);
    if (s_output_snap_view != NULL) wgpuTextureViewRelease(s_output_snap_view);
    if (s_white_view != NULL) wgpuTextureViewRelease(s_white_view);
    if (s_resolve_tex != NULL) wgpuTextureRelease(s_resolve_tex);
    if (s_output_depth_tex != NULL) wgpuTextureRelease(s_output_depth_tex);
    if (s_scene_tex != NULL) wgpuTextureRelease(s_scene_tex);
    if (s_depth_tex != NULL) wgpuTextureRelease(s_depth_tex);
    if (s_post_tex != NULL) wgpuTextureRelease(s_post_tex);
    if (s_snap_tex != NULL) wgpuTextureRelease(s_snap_tex);
    if (s_output_snap_tex != NULL) wgpuTextureRelease(s_output_snap_tex);
    if (s_white_tex != NULL) wgpuTextureRelease(s_white_tex);

    if (s_resolve_ubuf != NULL) wgpuBufferRelease(s_resolve_ubuf);
    for (int i = 0; i < s_resolve_ubo_ext_cap; i++) {
        if (s_resolve_ubo_ext[i] != NULL) wgpuBufferRelease(s_resolve_ubo_ext[i]);
    }
    free(s_resolve_ubo_ext);
    s_resolve_ubo_ext = NULL;
    s_resolve_ubo_ext_cap = 0;
    s_resolve_ubo_used = 0;
    if (s_post_ubuf != NULL) wgpuBufferRelease(s_post_ubuf);
    if (s_mm_ubuf != NULL) wgpuBufferRelease(s_mm_ubuf);
    for (int i = 0; i < WGPU_DIAG_UBO_RING; i++) {
        if (s_diag_ubo[i] != NULL) wgpuBufferRelease(s_diag_ubo[i]);
    }
    for (int i = 0; i < s_diag_ubo_ext_cap; i++) {
        if (s_diag_ubo_ext[i].buf != NULL) {
            wgpuBufferRelease(s_diag_ubo_ext[i].buf);
        }
    }
    if (s_vbuf != NULL) wgpuBufferRelease(s_vbuf);
    for (int i = 0; i < WGPU_NOISE_UBO_RING; i++) {
        if (s_noise_ubo[i] != NULL) wgpuBufferRelease(s_noise_ubo[i]);
    }
    for (int i = 0; i < s_noise_ubo_ext_cap; i++) {
        if (s_noise_ubo_ext[i].buf != NULL) {
            wgpuBufferRelease(s_noise_ubo_ext[i].buf);
        }
    }
    if (s_light_ubo != NULL) wgpuBufferRelease(s_light_ubo);
    if (s_modern_ubo != NULL) wgpuBufferRelease(s_modern_ubo);

    if (s_resolve_samp != NULL) wgpuSamplerRelease(s_resolve_samp);
    if (s_post_sampN != NULL) wgpuSamplerRelease(s_post_sampN);
    if (s_post_sampL != NULL) wgpuSamplerRelease(s_post_sampL);
    if (s_snap_sampler != NULL) wgpuSamplerRelease(s_snap_sampler);
    if (s_default_sampler != NULL) wgpuSamplerRelease(s_default_sampler);
    if (s_modern_sampler != NULL) wgpuSamplerRelease(s_modern_sampler);
    for (int i = 0; i < s_sampler_n; i++) {
        if (s_samplers[i].sampler != NULL) {
            wgpuSamplerRelease(s_samplers[i].sampler);
        }
    }

    s_resolve_pipe = NULL;
    s_resolve_bgl = NULL;
    s_resolve_ubuf = NULL;
    s_resolve_samp = NULL;
    s_resolve_tex = NULL;
    s_resolve_view = NULL;
    s_output_depth_tex = NULL;
    s_output_depth_view = NULL;
    s_scene_tex = NULL;
    s_scene_view = NULL;
    s_depth_tex = NULL;
    s_depth_view = NULL;
    s_post_tex = NULL;
    s_post_view = NULL;
    s_present_target_view = NULL;
    s_present_target_tex = NULL;
    s_encoder = NULL;
    s_pass = NULL;
    s_overlay_pass = NULL;
    s_snap_tex = NULL;
    s_snap_view = NULL;
    s_output_snap_tex = NULL;
    s_output_snap_view = NULL;
    s_snap_sampler = NULL;
    memset(s_diag_ubo, 0, sizeof(s_diag_ubo));
    memset(s_diag_ubo_val, 0, sizeof(s_diag_ubo_val));
    if (s_diag_ubo_ext != NULL) {
        memset(s_diag_ubo_ext, 0,
               (size_t)s_diag_ubo_ext_cap * sizeof(*s_diag_ubo_ext));
    }
    s_diag_ubo_used = 0;
    s_white_tex = NULL;
    s_white_view = NULL;
    s_default_sampler = NULL;
    s_vbuf = NULL;
    s_vbuf_cap = 0;
    s_vbuf_off = 0;
    s_vbuf_frame_bytes = 0;
    s_vbuf_frame_segments = 0;
    memset(s_noise_ubo, 0, sizeof(s_noise_ubo));
    memset(s_noise_ubo_val, 0, sizeof(s_noise_ubo_val));
    if (s_noise_ubo_ext != NULL) {
        memset(s_noise_ubo_ext, 0,
               (size_t)s_noise_ubo_ext_cap * sizeof(*s_noise_ubo_ext));
    }
    s_noise_ubo_used = 0;
    s_light_ubo = NULL;
    s_modern_ubo = NULL;
    s_modern_ubo_cap = 0;
    s_modern_ubo_used = 0;
    s_modern_ubo_gen++;
    s_post_pipe = NULL;
    s_post_bgl = NULL;
    s_post_ubuf = NULL;
    s_post_sampN = NULL;
    s_post_sampL = NULL;
    s_post_bg = NULL;
    s_post_bg_view = NULL;

    for (size_t i = 0; i < s_shader_count; i++) {
        free(s_shaders[i]);
    }
    free(s_shaders);
    s_shaders = NULL;
    s_shader_count = 0;
    s_shader_capacity = 0;
    s_pipeline_slots_live = 0;
    s_cur_shader = NULL;
    memset(s_bg_cache_tab, 0, sizeof(s_bg_cache_tab));
    s_bg_cache_way = 0;
    s_bg_cache_live = 0;

    if (s_tex != NULL) {
        memset(s_tex, 0, (size_t)s_tex_cap * sizeof(*s_tex));
    }
    s_tex_hi = 0;
    s_tex_free_n = 0;
    memset(s_bound_tex, 0, sizeof(s_bound_tex));
    memset(s_bound_sampler, 0, sizeof(s_bound_sampler));
    s_active_tile = 0;
    memset(s_samplers, 0, sizeof(s_samplers));
    s_sampler_n = 0;

    s_mm_pipe = NULL;
    s_mm_bgl = NULL;
    s_mm_bg = NULL;
    s_mm_ubuf = NULL;
    s_modern_mod = NULL;
    s_modern_bgl = NULL;
    s_modern_pl = NULL;
    memset(s_modern_pipe, 0, sizeof(s_modern_pipe));
    s_modern_sampler = NULL;
    memset(s_modern_cache, 0, sizeof(s_modern_cache));
    s_modern_count = 0;

    s_cfg_w = 0;
    s_cfg_h = 0;
    s_scene_w = 0;
    s_scene_h = 0;
    s_resolve_w = 0;
    s_resolve_h = 0;
    s_snap_w = 0;
    s_snap_h = 0;
    s_output_snap_w = 0;
    s_output_snap_h = 0;
    s_resize_pending_w = 0;
    s_resize_pending_h = 0;
    s_resize_stable = 0;
    s_surface_copy_dst = false;
    s_surface_copy_src = false;
    s_surface_recovery_attempts = 0;
    s_frame_open = false;
    s_output_overlay_active = false;
    s_pending_pipelines = 0;
    s_frame_pending_skips = 0;
    s_present_hold_streak = 0;
    wgpu_reset_pass_dynamic_state();
}

static void wgpu_shutdown(void) {
    const bool owned_roots = s_owns_device;
    const size_t shaders = s_shader_count;
    const uint32_t textures = s_tex_hi;
    const int pending = s_pending_pipelines;
    const bool shadow_resource_latched =
        s_shadow_resource_perma_fail;

    if (s_gpu_frames_in_flight > 0u && WGPU_COMPAT_QUEUE_CAN_BLOCK) {
        /* Native has a blocking queue pump, so its orderly teardown must
         * observe every submitted completion before releasing device roots. */
        (void)wgpu_backpressure_check_below(1u, true, false);
    }
    if (s_gpu_frames_in_flight > 0u) {
        /* A browser completion callback needs a future event-loop turn, but
         * shutdown can be reached from a non-yielding host callback.  Poll
         * once at the renderer-owned boundary, then account for any residual
         * submitted work as orderly teardown—not as a rejected visual frame.
         * Sleeping here can strand Asyncify during page/process teardown. */
        wgpu_pipeline_callback_owner_poll();
        if (s_gpu_frames_in_flight > 0u) {
            fprintf(stderr,
                    "[webgpu] shutdown retiring %u submitted frame(s) "
                    "without a completion callback\n",
                    s_gpu_frames_in_flight);
            wgpu_abandon_in_flight();
        }
    }
    wgpu_report_backpressure();
    s_ready = false;
    s_runtime_status = GFX_RENDERING_UNINITIALIZED;
    /* Invalidate browser async callback contexts before any ShaderProgram is
     * freed. A late callback will release only its returned pipeline + context. */
    s_active_work_generation = 0;
    wgpu_prewarm_flush();
    if (s_pending_pipelines > 0) {
        wgpu_pipeline_callback_owner_drain();
    }
    /* A browser promise may still be pending after a non-yielding shutdown
     * drain. Its heap context is intentionally retained for that completion;
     * s_active_work_generation is already zero, so the callback can only
     * release its returned pipeline and free the context (never its old prg). */
    if (s_pending_pipelines > 0) {
#ifdef __EMSCRIPTEN__
        s_pipeline_callback_shutdown_guarded +=
            (uint64_t)s_pending_pipelines;
#endif
        fprintf(stderr,
                "[webgpu] shutdown guarded %d late pipeline completion(s) "
                "by generation\n", s_pending_pipelines);
    }

    wgpu_release_device_objects();
    if (s_surface != NULL && owned_roots) {
        wgpuSurfaceUnconfigure(s_surface);
        wgpuSurfaceRelease(s_surface);
    }
    if (owned_roots) {
        gfx_webgpu_host_device_will_release(s_device);
        if (s_device != NULL) {
            wgpuDeviceDestroy(s_device);
        }
        if (s_queue != NULL) wgpuQueueRelease(s_queue);
        if (s_device != NULL) wgpuDeviceRelease(s_device);
        if (s_adapter != NULL) wgpuAdapterRelease(s_adapter);
        if (s_instance != NULL) wgpuInstanceRelease(s_instance);
    }

    /*
     * Host-adopted roots are borrowed: do not unconfigure, destroy, or release
     * them. The backend still drops every child above and forgets all borrows.
     */
    s_surface = NULL;
    s_queue = NULL;
    s_device = NULL;
    s_adapter = NULL;
    s_instance = NULL;
    s_surface_format = WGPUTextureFormat_Undefined;
    s_owns_device = false;
    s_unclipped_depth_supported = false;

    free(s_tex);
    s_tex = NULL;
    s_tex_cap = 0;
    s_tex_hi = 0;
    free(s_tex_free);
    s_tex_free = NULL;
    s_tex_free_n = 0;
    s_tex_free_cap = 0;
    free(s_diag_ubo_ext);
    s_diag_ubo_ext = NULL;
    s_diag_ubo_ext_cap = 0;
    free(s_vbuf_shadow);
    s_vbuf_shadow = NULL;
    s_vbuf_shadow_cap = 0;
    free(s_shadow_upload);
    s_shadow_upload = NULL;
    s_shadow_upload_cap = 0;
    free(s_vp_fix_buf);
    s_vp_fix_buf = NULL;
    s_vp_fix_cap = 0;
    s_pending_pipelines = 0;
    s_frame_pending_skips = 0;
    s_active_work_generation = 0;

    fprintf(stderr,
            "[WGPU-SHUTDOWN] roots=%s shaders=%zu textures=%u "
            "pendingPipelines=%d liveChildren=0 cpuArrays=0\n",
            owned_roots ? "owned" : "borrowed",
            shaders, textures, pending);
    fprintf(stderr,
            "[WORLD-SHADOW] backend=webgpu attempted=%llu complete=%llu "
            "fallback=%llu resourceFailures=%llu latched=%d\n",
            (unsigned long long)s_shadow_attempted_frames,
            (unsigned long long)s_shadow_complete_frames,
            (unsigned long long)s_shadow_fallback_frames,
            (unsigned long long)s_shadow_resource_failures,
            shadow_resource_latched ? 1 : 0);
    fprintf(stderr,
            "[WGPU-PERF] asyncCreates=%llu asyncReady=%llu asyncFailed=%llu "
            "holdFrames=%llu maxHoldStreak=%u maxPipelineFrames=%u "
            "maxPending=%u ownerEventPumps=%llu lateSafe=%llu "
            "shutdownGuarded=%llu\n",
            (unsigned long long)s_async_pipeline_creates,
            (unsigned long long)s_async_pipeline_ready,
            (unsigned long long)s_async_pipeline_failed,
            (unsigned long long)s_async_present_hold_frames,
            s_present_hold_streak_max, s_async_pipeline_frames_max,
            s_async_pending_high_water,
#ifdef __EMSCRIPTEN__
            (unsigned long long)s_pipeline_callback_owner_event_pumps,
            (unsigned long long)s_pipeline_callback_shutdown_late_safe,
            (unsigned long long)s_pipeline_callback_shutdown_guarded);
#else
            0ull, 0ull, 0ull);
#endif
}

bool gfx_webgpu_recover_device(void) {
#ifdef __EMSCRIPTEN__
    return false;
#else
    if (!gfx_webgpu_runtime_recovery_pending()) {
        return false;
    }
    const bool borrowed_roots = !s_owns_device;
    if (s_native_recovery_attempted) {
        fprintf(stderr,
                "[webgpu] native recovery already attempted; stopping after "
                "the repeated device failure\n");
        if (borrowed_roots) {
            /* AppHost owns the user-visible terminal state as well as the
             * borrowed roots. Record a durable launcher error before the
             * engine unwinds; stderr alone is not an adequate failure UI. */
            (void)platformRecoverHostWebGpu(HOST_WEBGPU_RECOVERY_ABORT);
        }
        return false;
    }
    s_native_recovery_attempted = true;
    /* The failed generation cannot contribute to the new device's queue cap.
     * Its late callbacks carry the old generation token and are ignored. */
    wgpu_abandon_in_flight();
    {
        const char *force_failure =
            getenv("MDKR_TEST_WEBGPU_RECOVERY_FAIL");
        if (force_failure != NULL && force_failure[0] == '1') {
            fprintf(stderr, "[webgpu] injected native recovery failure\n");
            if (borrowed_roots) {
                (void)platformRecoverHostWebGpu(
                    HOST_WEBGPU_RECOVERY_ABORT);
            }
            return false;
        }
    }

    WGPUInstance instance = NULL;
    WGPUAdapter adapter = NULL;
    WGPUDevice device = NULL;
    WGPUQueue queue = NULL;
    WGPUSurface surface = NULL;
    int format = 0;
    void *layer = NULL;
#ifdef __APPLE__
    layer = platformGetMetalLayer();
#endif
    fprintf(stderr,
            "[webgpu] attempting one native device reinitialization (%s roots)\n",
            borrowed_roots ? "AppHost-owned" : "engine-owned");
    if (borrowed_roots) {
        if (!platformRecoverHostWebGpu(HOST_WEBGPU_RECOVERY_PREPARE)) {
            (void)platformRecoverHostWebGpu(HOST_WEBGPU_RECOVERY_ABORT);
            return false;
        }
    } else if (!gfx_webgpu_bringup(
                   layer, platformGetSdlWindow(), &instance, &adapter, &device,
                   &queue, &surface, &format)) {
        return false;
    }
    /* The new device has a new immutable callback token. Give work callbacks a
     * distinct session token before destroying the old queue/device so their
     * delayed completions cannot mutate the rebuilt renderer. */
    s_active_work_generation = ++s_next_device_generation;

    WGPUInstance old_instance = s_instance;
    WGPUAdapter old_adapter = s_adapter;
    WGPUDevice old_device = s_device;
    WGPUQueue old_queue = s_queue;
    WGPUSurface old_surface = s_surface;

    wgpu_release_device_objects();
    if (borrowed_roots) {
        /* AppHost owns both root sets. COMMIT runs only after every engine child
         * above has been released, swaps the bridge as one transaction, then
         * destroys the failed roots exactly once. */
        if (!platformRecoverHostWebGpu(HOST_WEBGPU_RECOVERY_COMMIT)) {
            (void)platformRecoverHostWebGpu(HOST_WEBGPU_RECOVERY_ABORT);
            return false;
        }
        instance = (WGPUInstance)platformHostWgpuInstance();
        adapter = (WGPUAdapter)platformHostWgpuAdapter();
        device = (WGPUDevice)platformHostWgpuDevice();
        queue = (WGPUQueue)platformHostWgpuQueue();
        surface = (WGPUSurface)platformHostWgpuSurface();
        format = platformHostWgpuSurfaceFormat();
        if (instance == NULL || adapter == NULL || device == NULL ||
            queue == NULL || surface == NULL ||
            format == (int)WGPUTextureFormat_Undefined) {
            fprintf(stderr,
                    "[webgpu] AppHost committed an incomplete replacement\n");
            (void)platformRecoverHostWebGpu(HOST_WEBGPU_RECOVERY_ABORT);
            return false;
        }
    } else {
        if (old_surface != NULL) {
            wgpuSurfaceUnconfigure(old_surface);
            wgpuSurfaceRelease(old_surface);
        }
        if (old_device != NULL) wgpuDeviceDestroy(old_device);
        if (old_queue != NULL) wgpuQueueRelease(old_queue);
        if (old_device != NULL) wgpuDeviceRelease(old_device);
        if (old_adapter != NULL) wgpuAdapterRelease(old_adapter);
        if (old_instance != NULL) wgpuInstanceRelease(old_instance);
    }

    s_instance = instance;
    s_adapter = adapter;
    s_device = device;
    s_queue = queue;
    s_surface = surface;
    s_surface_format = (WGPUTextureFormat)format;
    s_ready = true;
    s_runtime_status = GFX_RENDERING_READY;
    if (!wgpu_configure_surface(gfx_output_dimensions.width,
                                gfx_output_dimensions.height)) {
        s_ready = false;
        s_runtime_status = GFX_RENDERING_FATAL;
        if (borrowed_roots) {
            (void)platformRecoverHostWebGpu(HOST_WEBGPU_RECOVERY_ABORT);
        }
        return false;
    }
    s_callback_recovery_pending = false;
    s_runtime_status = GFX_RENDERING_READY;
    return true;
#endif
}

void gfx_webgpu_report_limits(void) {
    if (s_limits_reported) {
        return;
    }
    s_limits_reported = true;
    /*
     * Keep the historical `tableOverflow` field name because release gates and
     * archived traces consume it. The implementation is now a stable-pointer
     * growable index rather than the old overwrite-on-full table, so the value
     * means attempts rejected by the explicit shader safety guard.
     */
    fprintf(stderr,
            "[WGPU-LIMITS] shaders=%zu/%u pipelines=%zu attrs=%d/%u "
            "varyings=%d/%u tableOverflow=%u pipelineFailures=%u "
            "vertexBytes=%llu vertexSegments=%u vertexSegmentCap=%u "
            "textures=%u samplers=%d bindGroups=%u/%u\n",
            s_shader_high_water, WGPU_SHADER_HARD_LIMIT,
            s_pipeline_slots_high_water,
            s_max_attrs_seen, 16u, s_max_varyings_seen, 16u,
            s_shader_guard_hits, s_pipeline_failures,
            (unsigned long long)s_vbuf_bytes_high_water,
            s_vbuf_segments_high_water, s_vbuf_cap_high_water,
            s_tex_high_water, s_sampler_high_water,
            s_bg_cache_high_water, WGPU_BG_CACHE);

    const char *census = getenv("MDKR_WGPU_CENSUS");
    if (census == NULL || census[0] != '1') {
        return;
    }
    size_t max_pipelines = 0;
    uint64_t max_shader_id0 = 0;
    uint32_t max_shader_id1 = 0;
    for (size_t i = 0; i < s_shader_count; i++) {
        const struct ShaderProgram *prg = s_shaders[i];
        if (prg == NULL) {
            continue;
        }
        if ((size_t)prg->npipes > max_pipelines) {
            max_pipelines = (size_t)prg->npipes;
            max_shader_id0 = prg->shader_id0;
            max_shader_id1 = prg->shader_id1;
        }
        fprintf(stderr,
                "[WGPU-MATERIAL] id=%016llx/%08x attrs=%d varyings=%d "
                "pipelines=%d/%u keys=",
                (unsigned long long)prg->shader_id0,
                (unsigned)prg->shader_id1,
                prg->info.num_attrs,
                prg->info.num_attrs > 0 ? prg->info.num_attrs - 1 : 0,
                prg->npipes, WGPU_PIPE_CACHE);
        for (int j = 0; j < prg->npipes; j++) {
            fprintf(stderr, "%s%03x:%u", j == 0 ? "" : ",",
                    (unsigned)prg->pipes[j].key,
                    (unsigned)prg->pipes[j].state);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr,
            "[WGPU-CENSUS] materials=%zu maxPipelines=%zu/%u "
            "maxPipelineShader=%016llx/%08x\n",
            s_shader_count, max_pipelines, WGPU_PIPE_CACHE,
            (unsigned long long)max_shader_id0,
            (unsigned)max_shader_id1);
}

/* ------------------------------------------------------------------------
 * The vtable — same field order as gfx_opengl_api / gfx_metal_api.
 * ---------------------------------------------------------------------- */
struct GfxRenderingAPI gfx_webgpu_api = {
    .z_is_from_0_to_1 = wgpu_z_is_from_0_to_1,
    .unload_shader = wgpu_unload_shader,
    .load_shader = wgpu_load_shader,
    .create_and_load_new_shader = wgpu_create_and_load_new_shader,
    .lookup_shader = wgpu_lookup_shader,
    .shader_get_info = wgpu_shader_get_info,
    .new_texture = wgpu_new_texture,
    .delete_texture = wgpu_delete_texture,
    .select_texture = wgpu_select_texture,
    .upload_texture = wgpu_upload_texture,
    .set_sampler_parameters = wgpu_set_sampler_parameters,
    .set_depth_mode = wgpu_set_depth_mode,
    .set_viewport = wgpu_set_viewport,
    .set_scissor = wgpu_set_scissor,
    .set_shadow_view = wgpu_set_shadow_view,
    .set_blend_mode = wgpu_set_blend_mode,
    .draw_triangles = wgpu_draw_triangles,
    .read_framebuffer_rgb = wgpu_read_framebuffer_rgb,
    .init = wgpu_init,
    .on_resize = wgpu_on_resize,
    .start_frame = wgpu_start_frame,
    .begin_output_overlay = wgpu_begin_output_overlay,
    .end_frame = wgpu_end_frame,
    .finish_render = wgpu_finish_render,
    .draw_modern_mesh = wgpu_draw_modern_mesh,
    .upload_texture_mipped = wgpu_upload_texture_mipped,
    .shutdown = wgpu_shutdown,
    .get_status = wgpu_get_status,
};
