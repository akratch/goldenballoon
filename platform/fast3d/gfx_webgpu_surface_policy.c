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
