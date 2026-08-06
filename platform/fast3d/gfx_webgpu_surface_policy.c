#include "gfx_webgpu_surface_policy.h"

uint32_t gfx_webgpu_surface_select_alpha(
    uint32_t automatic_mode,
    uint32_t opaque_mode,
    bool opaque_advertised) {
    return opaque_advertised ? opaque_mode : automatic_mode;
}

uint32_t gfx_webgpu_surface_select_present(
    uint32_t fifo_mode,
    uint32_t requested_mode,
    bool requested_advertised) {
    return requested_advertised ? requested_mode : fifo_mode;
}

GfxWebgpuPresentMode gfx_webgpu_surface_request_present(
    MdkrPresentSync sync,
    bool allow_tearing) {
    if (allow_tearing) {
        return GFX_WEBGPU_PRESENT_IMMEDIATE;
    }
    return sync == MDKR_PRESENT_SYNC_LATEST ? GFX_WEBGPU_PRESENT_MAILBOX
                                            : GFX_WEBGPU_PRESENT_FIFO;
}

GfxWebgpuPresentMode gfx_webgpu_surface_rank_present(
    MdkrPresentSync sync,
    bool allow_tearing,
    GfxWebgpuPresentSupport advertised) {
    if (allow_tearing && advertised.immediate) {
        return GFX_WEBGPU_PRESENT_IMMEDIATE;
    }
    if (sync == MDKR_PRESENT_SYNC_LATEST && advertised.mailbox) {
        return GFX_WEBGPU_PRESENT_MAILBOX;
    }
    return GFX_WEBGPU_PRESENT_FIFO;
}

const char *gfx_webgpu_surface_present_name(GfxWebgpuPresentMode mode) {
    switch (mode) {
        case GFX_WEBGPU_PRESENT_MAILBOX:   return "mailbox";
        case GFX_WEBGPU_PRESENT_IMMEDIATE: return "immediate";
        case GFX_WEBGPU_PRESENT_FIFO:
        default:                           return "fifo";
    }
}
