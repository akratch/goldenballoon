#include "gfx_webgpu_lifecycle.h"

#include <string.h>

#define MDKR_WGPU_MAX_SUBOPTIMAL_FRAMES 30u
#define MDKR_WGPU_MAX_CONSECUTIVE_ERRORS 60u

void mdkr_wgpu_lifecycle_init(struct MdkrWgpuLifecycle *state) {
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

enum MdkrWgpuSurfaceAction mdkr_wgpu_lifecycle_acquire(
    struct MdkrWgpuLifecycle *state, enum MdkrWgpuAcquireStatus status) {
    if (state == NULL) {
        return MDKR_WGPU_SURFACE_RECOVER_DEVICE;
    }

    switch (status) {
        case MDKR_WGPU_ACQUIRE_OPTIMAL:
            state->consecutive_errors = 0;
            state->suboptimal_frames = 0;
            if (state->reconfigure_pending) {
                state->reconfigure_pending = false;
                return MDKR_WGPU_SURFACE_PRESENT_RECONFIGURE;
            }
            return MDKR_WGPU_SURFACE_PRESENT;

        case MDKR_WGPU_ACQUIRE_SUBOPTIMAL:
            state->consecutive_errors = 0;
            state->suboptimal_frames++;
            if (state->suboptimal_frames >= MDKR_WGPU_MAX_SUBOPTIMAL_FRAMES) {
                /* The returned action includes the repair; do not schedule it
                 * a second time on the following optimal frame. */
                state->reconfigure_pending = false;
                state->suboptimal_frames = 0;
                return MDKR_WGPU_SURFACE_PRESENT_RECONFIGURE;
            }
            state->reconfigure_pending = true;
            return MDKR_WGPU_SURFACE_PRESENT;

        case MDKR_WGPU_ACQUIRE_TIMEOUT:
            /* A timeout is transient back-pressure, not evidence of loss. */
            return MDKR_WGPU_SURFACE_SKIP;

        case MDKR_WGPU_ACQUIRE_OUTDATED:
            state->reconfigure_pending = false;
            state->suboptimal_frames = 0;
            return MDKR_WGPU_SURFACE_RECONFIGURE;

        case MDKR_WGPU_ACQUIRE_LOST:
            state->reconfigure_pending = false;
            state->suboptimal_frames = 0;
            state->consecutive_errors = 0;
            return MDKR_WGPU_SURFACE_RECREATE;

        case MDKR_WGPU_ACQUIRE_ERROR:
        default:
            state->consecutive_errors++;
            return state->consecutive_errors >= MDKR_WGPU_MAX_CONSECUTIVE_ERRORS
                ? MDKR_WGPU_SURFACE_RECOVER_DEVICE
                : MDKR_WGPU_SURFACE_SKIP;
    }
}

bool mdkr_wgpu_surface_policy(struct MdkrWgpuSurfacePolicy *out,
                              uint64_t advertised_usages,
                              uint64_t render_attachment_bit,
                              uint64_t copy_dst_bit,
                              uint64_t copy_src_bit,
                              bool request_surface_readback) {
    struct MdkrWgpuSurfacePolicy policy;
    if (out == NULL || render_attachment_bit == 0 ||
        (advertised_usages & render_attachment_bit) == 0) {
        return false;
    }

    memset(&policy, 0, sizeof(policy));
    policy.advertised_usages = advertised_usages;
    policy.configured_usages = render_attachment_bit;
    policy.copy_present = (advertised_usages & copy_dst_bit) != 0;
    policy.render_blit_present = !policy.copy_present;
    if (policy.copy_present) {
        policy.configured_usages |= copy_dst_bit;
    }
    policy.surface_readback =
        request_surface_readback && (advertised_usages & copy_src_bit) != 0;
    if (policy.surface_readback) {
        policy.configured_usages |= copy_src_bit;
    }
    *out = policy;
    return true;
}

bool mdkr_wgpu_shader_layout_supported(uint32_t attribute_count,
                                       uint32_t varying_count,
                                       uint32_t attribute_storage_capacity,
                                       uint32_t device_attribute_limit,
                                       uint32_t device_varying_limit) {
    return attribute_count > 0 &&
           attribute_count <= attribute_storage_capacity &&
           attribute_count <= device_attribute_limit &&
           varying_count <= device_varying_limit;
}
