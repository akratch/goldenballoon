/*
 * gfx_webgpu_lifecycle.h — ROM-free policy for WebGPU surface/device recovery.
 *
 * Keep the state transitions independent of webgpu.h so every status and
 * failure sequence can be exhaustively unit tested without a GPU or window.
 */
#ifndef GFX_WEBGPU_LIFECYCLE_H
#define GFX_WEBGPU_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

enum MdkrWgpuAcquireStatus {
    MDKR_WGPU_ACQUIRE_OPTIMAL = 0,
    MDKR_WGPU_ACQUIRE_SUBOPTIMAL,
    MDKR_WGPU_ACQUIRE_TIMEOUT,
    MDKR_WGPU_ACQUIRE_OUTDATED,
    MDKR_WGPU_ACQUIRE_LOST,
    MDKR_WGPU_ACQUIRE_ERROR,
};

enum MdkrWgpuSurfaceAction {
    MDKR_WGPU_SURFACE_PRESENT = 0,
    MDKR_WGPU_SURFACE_PRESENT_RECONFIGURE,
    MDKR_WGPU_SURFACE_SKIP,
    MDKR_WGPU_SURFACE_RECONFIGURE,
    MDKR_WGPU_SURFACE_RECREATE,
    MDKR_WGPU_SURFACE_RECOVER_DEVICE,
};

struct MdkrWgpuLifecycle {
    unsigned consecutive_errors;
    unsigned suboptimal_frames;
    bool reconfigure_pending;
};

struct MdkrWgpuSurfacePolicy {
    uint64_t advertised_usages;
    uint64_t configured_usages;
    bool copy_present;
    bool render_blit_present;
    bool surface_readback;
};

void mdkr_wgpu_lifecycle_init(struct MdkrWgpuLifecycle *state);
enum MdkrWgpuSurfaceAction mdkr_wgpu_lifecycle_acquire(
    struct MdkrWgpuLifecycle *state, enum MdkrWgpuAcquireStatus status);

/*
 * RenderAttachment is mandatory for a usable surface. CopyDst is preferred for
 * the exact texture-copy path; when absent, render_blit_present selects the
 * fullscreen render-pass fallback. CopySrc is optional and never requested
 * unless both advertised and needed.
 */
bool mdkr_wgpu_surface_policy(struct MdkrWgpuSurfacePolicy *out,
                              uint64_t advertised_usages,
                              uint64_t render_attachment_bit,
                              uint64_t copy_dst_bit,
                              uint64_t copy_src_bit,
                              bool request_surface_readback);

/*
 * Validate generated combiner metadata before copying it or submitting WGSL.
 * Kept independent of webgpu.h so the limit boundary is ROM-free and testable.
 */
bool mdkr_wgpu_shader_layout_supported(uint32_t attribute_count,
                                       uint32_t varying_count,
                                       uint32_t attribute_storage_capacity,
                                       uint32_t device_attribute_limit,
                                       uint32_t device_varying_limit);

#endif
