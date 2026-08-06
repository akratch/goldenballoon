#ifndef GFX_WEBGPU_SURFACE_POLICY_H
#define GFX_WEBGPU_SURFACE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "pacing_policy.h"

/* Pure surface-capability selection shared by production configuration and the
 * generation-recovery unit test. Values are uint32_t so this helper remains
 * independent of a particular WebGPU header dialect; the caller derives the
 * advertised booleans from the current generation's capability query. */
uint32_t gfx_webgpu_surface_select_alpha(
    uint32_t automatic_mode,
    uint32_t opaque_mode,
    bool opaque_advertised);

uint32_t gfx_webgpu_surface_select_present(
    uint32_t fifo_mode,
    uint32_t requested_mode,
    bool requested_advertised);

/* Backend-neutral present modes, ranked here and mapped to the header dialect's
 * WGPUPresentMode by the caller. FIFO is the only one a surface must advertise;
 * the other two are resolved against the active generation's capabilities. */
typedef enum GfxWebgpuPresentMode {
    GFX_WEBGPU_PRESENT_FIFO = 0,
    GFX_WEBGPU_PRESENT_MAILBOX = 1,
    GFX_WEBGPU_PRESENT_IMMEDIATE = 2
} GfxWebgpuPresentMode;

typedef struct GfxWebgpuPresentSupport {
    bool mailbox;
    bool immediate;
} GfxWebgpuPresentSupport;

/*
 * Rank the modes a latched policy can accept against what the surface offers.
 * Immediate is the only mode that scans out a partial frame, so it is reachable
 * only through `allow_tearing`; a LATEST policy that cannot get mailbox lands on
 * FIFO rather than tearing its way to the requested rate.
 */
GfxWebgpuPresentMode gfx_webgpu_surface_rank_present(
    MdkrPresentSync sync,
    bool allow_tearing,
    GfxWebgpuPresentSupport advertised);

/* The mode a policy asks for before capabilities are consulted, so the
 * [PRESENT-MODE] row can report requested and effective separately. */
GfxWebgpuPresentMode gfx_webgpu_surface_request_present(
    MdkrPresentSync sync,
    bool allow_tearing);

const char *gfx_webgpu_surface_present_name(GfxWebgpuPresentMode mode);

#endif
